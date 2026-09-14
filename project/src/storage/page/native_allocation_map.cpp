// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_allocation_map.hpp"
#include "disk_device.hpp"
#include "filespace_page_zero.hpp"
#include "hash_digest_parts.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
namespace hash = scratchbird::core::hash;
using E = NativeAllocationError;
using S = NativeAllocationState;
constexpr std::size_t start = 384, seal = 312, record_bytes = 128;
bool Zero(const byte* p, std::size_t n) {
  return std::all_of(p, p + n, [](byte b) { return b == 0; });
}
bool V7(const Uuid& id) {
  return !id.is_nil() && (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}
Uuid GetUuid(const byte* p) { Uuid id; std::copy_n(p, 16, id.bytes.begin()); return id; }
void PutUuid(byte* p, const Uuid& id) { std::copy(id.bytes.begin(), id.bytes.end(), p); }
disk::NativePageReference GetRef(const byte* p) {
  return {GetUuid(p), LoadLittle64(p + 16), LoadLittle64(p + 24), GetUuid(p + 32)};
}
void PutRef(byte* p, const disk::NativePageReference& ref) {
  PutUuid(p, ref.filespace_uuid); StoreLittle64(p + 16, ref.page_number);
  StoreLittle64(p + 24, ref.page_generation); PutUuid(p + 32, ref.page_size_profile_uuid);
}
NativeAllocationMapResult Fail(E error) { NativeAllocationMapResult result; result.error = error; return result; }
NativeAllocationChainResult ChainFail(E error) { NativeAllocationChainResult result; result.error = error; return result; }
auto Digest(const std::vector<byte>& bytes, bool zero_seal) {
  const std::array<byte, 32> zero{};
  const hash::HashDigestSegment parts[] = {
      {bytes.data(), seal}, {zero_seal ? zero.data() : bytes.data() + seal, 32},
      {bytes.data() + seal + 32, bytes.size() - seal - 32}};
  return hash::ComputeSha256DigestParts(parts, 3);
}
std::size_t RecordsAt(std::size_t count) { return (start + (count + 1) / 2 + 7) & ~std::size_t(7); }
E Validate(const NativeAllocationMap& map) {
  const auto& h = map.header;
  if (!disk::EncodeNativeCommonPageHeader(h).ok() || h.page_type != 3 || (h.flags & ~u64(2)))
    return E::invalid_header;
  if (!V7(map.object_uuid) || !V7(map.creator_transaction_uuid)) return E::invalid_identity;
  if (!map.map_generation || !map.capacity_generation || !map.creator_local_transaction_id)
    return E::invalid_family;
  if (!map.total_pages || map.total_pages > std::numeric_limits<u64>::max() / h.page_size_bytes ||
      h.page_number >= map.total_pages || map.first_page >= map.total_pages || map.states.empty() ||
      map.states.size() > map.total_pages - map.first_page ||
      map.states.size() > (h.page_size_bytes - start) * 2u) return E::invalid_range;
  const auto records_at = RecordsAt(map.states.size());
  if (records_at > h.page_size_bytes || map.records.size() > (h.page_size_bytes - records_at) / record_bytes)
    return E::invalid_range;
  const bool terminal = map.states.size() == map.total_pages - map.first_page;
  if (terminal == map.next.has_value() || map.next.has_value() == Zero(map.next_sha256.data(), 32))
    return E::invalid_reference;
  if (map.next) {
    const auto& ref = *map.next;
    if (ref.filespace_uuid != h.filespace_uuid || ref.page_size_profile_uuid != h.page_size_profile_uuid ||
        !ref.page_number || ref.page_number >= map.total_pages || ref.page_number == h.page_number ||
        !ref.page_generation) return E::invalid_reference;
  }
  std::size_t record = 0;
  for (std::size_t i = 0; i < map.states.size(); ++i) {
    const auto state = map.states[i];
    if (static_cast<byte>(state) > 7) return E::invalid_state;
    const auto number = map.first_page + i;
    if (record < map.records.size() && map.records[record].page_number < number) return E::invalid_record;
    const bool present = record < map.records.size() && map.records[record].page_number == number;
    if (state == S::free) { if (present) return E::invalid_record; continue; }
    if (!present) { if (state != S::quarantined) return E::invalid_record; continue; }
    const auto& r = map.records[record++];
    if (!V7(r.allocation_uuid) || !V7(r.owner_uuid) || !V7(r.creator_transaction_uuid) ||
        (!r.page_uuid.is_nil() && !V7(r.page_uuid))) return E::invalid_identity;
    if (!r.creator_local_transaction_id || r.creator_local_transaction_id > map.creator_local_transaction_id ||
        !r.page_type || !disk::IsRegisteredNativePageType(r.page_type)) return E::invalid_record;
    if (r.page_uuid.is_nil() != (r.page_generation == 0)) return E::invalid_record;
    if (r.page_uuid.is_nil() && state != S::reserved && state != S::preallocated && state != S::quarantined)
      return E::invalid_record;
    if ((state == S::reusable_pending_mga || state == S::reusable_free) != (r.reuse_horizon != 0))
      return E::invalid_record;
  }
  return record == map.records.size() ? E::none : E::invalid_record;
}
}  // namespace

NativeAllocationMapResult EncodeNativeAllocationMap(const NativeAllocationMap& map) noexcept {
  try {
    const auto valid = Validate(map); if (valid != E::none) return Fail(valid);
    std::vector<byte> bytes(map.header.page_size_bytes, 0);
    const auto header = disk::EncodeNativeCommonPageHeader(map.header);
    if (!header.ok()) return Fail(E::invalid_header);
    std::copy(header.bytes->begin(), header.bytes->end(), bytes.begin());
    auto* f = bytes.data() + 128;
    std::copy_n("SBABM001", 8, f); StoreLittle16(f + 8, 1); StoreLittle16(f + 10, 256);
    const auto records_at = RecordsAt(map.states.size());
    StoreLittle32(f + 12, records_at + record_bytes * map.records.size());
    PutUuid(f + 16, map.object_uuid); StoreLittle64(f + 32, map.map_generation);
    StoreLittle64(f + 40, map.capacity_generation); StoreLittle64(f + 48, map.total_pages);
    StoreLittle64(f + 56, map.first_page); StoreLittle64(f + 64, map.states.size());
    PutUuid(f + 72, map.creator_transaction_uuid); StoreLittle64(f + 88, map.creator_local_transaction_id);
    if (map.next) PutRef(f + 96, *map.next);
    std::copy(map.next_sha256.begin(), map.next_sha256.end(), f + 144);
    StoreLittle32(f + 176, (map.states.size() + 1) / 2); StoreLittle32(f + 180, map.records.size());
    for (std::size_t i = 0; i < map.states.size(); ++i)
      bytes[start + i / 2] |= static_cast<byte>(map.states[i]) << (4 * (i % 2));
    for (std::size_t i = 0; i < map.records.size(); ++i) {
      auto* p = bytes.data() + records_at + record_bytes * i; const auto& r = map.records[i];
      StoreLittle64(p, r.page_number); PutUuid(p + 8, r.allocation_uuid); PutUuid(p + 24, r.page_uuid);
      PutUuid(p + 40, r.owner_uuid); PutUuid(p + 56, r.creator_transaction_uuid);
      StoreLittle64(p + 72, r.creator_local_transaction_id); StoreLittle64(p + 80, r.page_generation);
      StoreLittle64(p + 88, r.reuse_horizon); StoreLittle32(p + 96, r.page_type);
    }
    const auto digest = Digest(bytes, true); if (!digest.ok()) return Fail(E::hash_failure);
    std::copy(digest.digest.begin(), digest.digest.end(), bytes.begin() + seal);
    return {E::none, map, std::move(bytes)};
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_family); }
}

NativeAllocationMapResult DecodeNativeAllocationMap(const std::vector<byte>& bytes) noexcept {
  try {
    if (bytes.size() < start) return Fail(E::invalid_header);
    const auto header = disk::DecodeNativeCommonPageHeader(bytes.data(), 128);
    if (!header.ok() || header.header->page_type != 3 || header.header->page_size_bytes != bytes.size() ||
        (header.header->flags & ~u64(2))) return Fail(E::invalid_header);
    const auto digest = Digest(bytes, true); if (!digest.ok()) return Fail(E::hash_failure);
    if (!std::equal(digest.digest.begin(), digest.digest.end(), bytes.begin() + seal)) return Fail(E::invalid_integrity);
    const auto* f = bytes.data() + 128;
    const auto count = LoadLittle64(f + 64);
    const auto record_count = LoadLittle32(f + 180), used = LoadLittle32(f + 12);
    if (std::string_view(reinterpret_cast<const char*>(f), 8) != "SBABM001" ||
        LoadLittle16(f + 8) != 1 || LoadLittle16(f + 10) != 256 || !count ||
        count > (bytes.size() - start) * 2 || LoadLittle32(f + 176) != (count + 1) / 2 ||
        !Zero(f + 216, 40)) return Fail(E::invalid_family);
    const auto records_at = RecordsAt(static_cast<std::size_t>(count));
    if (records_at > bytes.size() || record_count > (bytes.size() - records_at) / record_bytes ||
        used != records_at + record_count * record_bytes ||
        !Zero(bytes.data() + start + (count + 1) / 2, records_at - start - (count + 1) / 2) ||
        !Zero(bytes.data() + used, bytes.size() - used) ||
        ((count & 1) && (bytes[start + count / 2] & 0xf0))) return Fail(E::invalid_family);
    NativeAllocationMap map; map.header = *header.header;
    map.object_uuid = GetUuid(f + 16); map.map_generation = LoadLittle64(f + 32);
    map.capacity_generation = LoadLittle64(f + 40); map.total_pages = LoadLittle64(f + 48);
    map.first_page = LoadLittle64(f + 56); map.creator_transaction_uuid = GetUuid(f + 72);
    map.creator_local_transaction_id = LoadLittle64(f + 88);
    if (!Zero(f + 96, 48)) map.next = GetRef(f + 96);
    std::copy_n(f + 144, 32, map.next_sha256.begin());
    map.states.reserve(count); map.records.reserve(record_count);
    for (std::size_t i = 0; i < count; ++i)
      map.states.push_back(static_cast<S>((bytes[start + i / 2] >> (4 * (i % 2))) & 15));
    for (u32 i = 0; i < record_count; ++i) {
      const auto* p = bytes.data() + records_at + record_bytes * i;
      if (!Zero(p + 100, 28)) return Fail(E::invalid_record);
      NativeAllocationRecord r; r.page_number = LoadLittle64(p); r.allocation_uuid = GetUuid(p + 8);
      r.page_uuid = GetUuid(p + 24); r.owner_uuid = GetUuid(p + 40); r.creator_transaction_uuid = GetUuid(p + 56);
      r.creator_local_transaction_id = LoadLittle64(p + 72); r.page_generation = LoadLittle64(p + 80);
      r.reuse_horizon = LoadLittle64(p + 88); r.page_type = LoadLittle32(p + 96); map.records.push_back(r);
    }
    const auto valid = Validate(map); if (valid != E::none) return Fail(valid);
    return {E::none, std::move(map), bytes};
  } catch (const std::bad_alloc&) { return Fail(E::resource_exhausted); }
    catch (const std::length_error&) { return Fail(E::resource_exhausted); }
    catch (...) { return Fail(E::invalid_family); }
}

static NativeAllocationChainResult ReadAllocationChain(
    disk::FileDevice& device, const disk::FilespaceBootstrapBinding& binding,
    const disk::FilespaceRootReference* selected,u64 maximum_retained_image_bytes) noexcept {
  try {
    if (!maximum_retained_image_bytes) return ChainFail(E::resource_exhausted);
    const auto guard = device.AcquireOperationGuard();
    const auto zero = disk::ReadFilespacePageZeroFromOpenDevice(device, &binding);
    if (!zero.ok()) {
      if (zero.error == disk::FilespacePageZeroError::resource_exhausted) return ChainFail(E::resource_exhausted);
      if (zero.error == disk::FilespacePageZeroError::hash_provider_failure) return ChainFail(E::hash_failure);
      if (zero.error == disk::FilespacePageZeroError::io_failure) return ChainFail(E::io_failure);
      return ChainFail(E::invalid_filespace);
    }
    const auto& z = *zero.record;
    if (z.bootstrap.flags & disk::FilespaceBootstrapFlag::cluster_authority_required)
      return ChainFail(E::cluster_requires_authority);
    const auto initial = std::find_if(z.roots.begin(), z.roots.end(), [](const auto& r) { return r.kind == 3; });
    const auto* root=selected?selected:initial==z.roots.end()?nullptr:&*initial;
    if (!root||root->kind!=3||root->page_type!=3||!V7(root->object_uuid)||!root->page_number||!root->page_generation||
        root->filespace_uuid != binding.filespace_uuid || root->page_size_profile_uuid != binding.page_size_profile_uuid) return ChainFail(E::invalid_reference);
    disk::NativePageReference ref{root->filespace_uuid, root->page_number, root->page_generation, root->page_size_profile_uuid};
    std::set<u64> slots; std::set<Uuid> page_ids{z.page_uuid};
    NativeAllocationChainResult result;
    std::array<byte, 32> expected_digest{};
    u64 covered = 0;
    for (;;) {
      if (!slots.insert(ref.page_number).second) return ChainFail(E::chain_mismatch);
      if (z.bootstrap.page_size_bytes > maximum_retained_image_bytes - result.retained_image_bytes)
        return ChainFail(E::resource_exhausted);
      if (ref.page_number >= z.total_pages) return ChainFail(E::invalid_range);
      std::vector<byte> bytes(z.bootstrap.page_size_bytes);
      const auto io = device.ReadAt(ref.page_number * z.bootstrap.page_size_bytes, bytes.data(), bytes.size());
      if (!io.ok() || io.bytes_transferred != bytes.size()) return ChainFail(E::io_failure);
      const disk::NativeCommonPageHeaderBinding expected{binding, ref.page_number, ref.page_generation, 3, {}};
      const auto common = disk::DecodeNativeCommonPageHeader(bytes.data(), 128, &expected);
      if (!common.ok()) return ChainFail(E::binding_mismatch);
      if (common.header->flags & 2) return ChainFail(E::cluster_requires_authority);
      if (!page_ids.insert(common.header->page_uuid).second) return ChainFail(E::chain_mismatch);
      if (!result.pages.empty()) {
        const auto digest = Digest(bytes, false); if (!digest.ok()) return ChainFail(E::hash_failure);
        if (digest.digest != expected_digest) return ChainFail(E::invalid_integrity);
      }
      auto decoded = DecodeNativeAllocationMap(bytes); if (!decoded.ok()) return ChainFail(decoded.error);
      const auto& map = *decoded.map;
      if (map.object_uuid != root->object_uuid || map.total_pages != z.total_pages || map.first_page != covered)
        return ChainFail(E::chain_mismatch);
      if (!result.pages.empty()) {
        const auto& head = *result.pages.front().map;
        if (map.map_generation != head.map_generation || map.capacity_generation != head.capacity_generation ||
            map.creator_transaction_uuid != head.creator_transaction_uuid ||
            map.creator_local_transaction_id != head.creator_local_transaction_id) return ChainFail(E::chain_mismatch);
      }
      covered += map.states.size();
      for (auto state : map.states) ++result.state_counts[static_cast<byte>(state)];
      result.retained_image_bytes += bytes.size();
      const auto next = map.next; expected_digest = map.next_sha256;
      result.pages.push_back(std::move(decoded));
      if (!next) break;
      ref = *next;
    }
    if (covered != z.total_pages || (!selected&&(result.state_counts[0] + result.state_counts[4] != z.free_pages ||
        result.state_counts[7] != z.preallocated_pages))) return ChainFail(E::counter_mismatch);
    const auto allocated = [&](u64 number, const Uuid& id, u64 generation, u32 type, const Uuid& owner) {
      for (const auto& image : result.pages) {
        const auto& map = *image.map;
        if (number < map.first_page || number - map.first_page >= map.states.size()) continue;
        if (map.states[number - map.first_page] != S::allocated) return false;
        const auto r = std::lower_bound(map.records.begin(), map.records.end(), number,
                                       [](const auto& value, u64 n) { return value.page_number < n; });
        return r != map.records.end() && r->page_number == number && r->page_uuid == id &&
               r->page_generation == generation && r->page_type == type && r->owner_uuid == owner;
      }
      return false;
    };
    const u32 zero_type = z.bootstrap.filespace_role <= 4 ? 1 : 2;
    if (!allocated(0, z.page_uuid, z.page_generation, zero_type, z.bootstrap.filespace_uuid))
      return ChainFail(E::physical_owner_mismatch);
    for (const auto& image : result.pages) {
      const auto& map = *image.map; const auto& h = map.header;
      if (!allocated(h.page_number, h.page_uuid, h.page_generation, 3, map.object_uuid))
        return ChainFail(E::physical_owner_mismatch);
    }
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) { return ChainFail(E::resource_exhausted); }
    catch (const std::length_error&) { return ChainFail(E::resource_exhausted); }
    catch (...) { return ChainFail(E::io_failure); }
}
NativeAllocationChainResult ReadNativeAllocationChainFromOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,u64 budget) noexcept {
  return ReadAllocationChain(device,binding,nullptr,budget);
}
NativeAllocationChainResult ReadNativeAllocationChainAtRootFromOpenDevice(
    disk::FileDevice& device,const disk::FilespaceBootstrapBinding& binding,
    const disk::FilespaceRootReference& root,u64 budget) noexcept {
  return ReadAllocationChain(device,binding,&root,budget);
}
}  // namespace scratchbird::storage::page
