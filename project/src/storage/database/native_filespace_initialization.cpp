// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_filespace_initialization.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E = NativeFilespaceInitializationError;
using S = page::NativeAllocationState;

NativeFilespaceInitializationResult Fail(E error) {
  NativeFilespaceInitializationResult result;
  result.error = error;
  return result;
}

bool V7(const Uuid& id) { return core::uuid::IsEngineIdentityUuid(id); }

// Reserve space for a record for every covered page, not only today's control
// pages. All admitted profiles are large enough for positive coverage.
u64 Coverage(u64 size) {
  u64 count = (size - 384) / 128;
  while (((384 + (count + 1) / 2 + 7) & ~u64{7}) + 128 * count > size) --count;
  return count;
}
}  // namespace

NativeFilespaceInitializationResult InitializeNativeFilespaceOnOpenDevice(
    disk::FileDevice& device, const NativeFilespaceInitializationRequest& request,
    u64 budget) noexcept {
  try {
    const auto guard = device.AcquireOperationGuard();
    const auto& b = request.bootstrap;
    if (disk::ValidateFilespaceBootstrap(b) != disk::FilespaceBootstrapError::none ||
        b.lifecycle_state != 7 || !V7(request.operation_uuid) || !V7(request.writer_uuid) ||
        !V7(request.creator.transaction_uuid.value) ||
        request.creator.transaction_uuid.kind != UuidKind::transaction ||
        !request.creator.local_id.value ||
        request.creator.scope != transaction::mga::TransactionScope::local_node ||
        !request.creation_utc_millis || request.creation_utc_millis > 0xffffffffffffULL)
      return Fail(E::invalid_request);
    if (b.flags & disk::FilespaceBootstrapFlag::cluster_authority_required)
      return Fail(E::cluster_requires_authority);
    if (!device.is_open() || device.read_only()) return Fail(E::invalid_device);
    const auto existing = device.Size();
    if (!existing.ok()) return Fail(E::io_failure);
    if (existing.size_bytes) return Fail(E::device_not_empty);

    const u64 size = b.page_size_bytes, coverage = Coverage(size), total = request.total_pages;
    if (!total || total > std::numeric_limits<u64>::max() / size ||
        !disk::CheckFileDeviceExtent(total * size, 0).ok())
      return Fail(E::invalid_capacity);
    const u64 count = total / coverage + (total % coverage != 0);
    if (count >= total) return Fail(E::invalid_capacity);
    if (budget / size < 2 || count > budget / size - 2 ||
        count > std::numeric_limits<std::size_t>::max() - 1)
      return Fail(E::resource_exhausted);

    // Resolve every identity and seal every metadata image before any write.
    std::set<Uuid> ids{b.database_uuid, b.filespace_uuid, b.page_size_profile_uuid,
                      b.checksum_profile_uuid, request.operation_uuid, request.writer_uuid,
                      request.creator.transaction_uuid.value};
    if (!b.encryption_profile_uuid.is_nil()) ids.insert(b.encryption_profile_uuid);
    const auto issue = [&](UuidKind kind) -> std::optional<Uuid> {
      const auto id = core::uuid::GenerateDurableEngineIdentityV7(kind, request.creation_utc_millis);
      if (!id.ok() || !ids.insert(id.value.value).second) return {};
      return id.value.value;
    };
    const auto zero_id = issue(UuidKind::page), map_id = issue(UuidKind::object);
    if (!zero_id || !map_id) return Fail(E::identity_failure);
    std::vector<Uuid> pages, allocations;
    pages.reserve(count);
    allocations.reserve(count + 1);
    for (u64 i = 0; i < count; ++i) {
      const auto id = issue(UuidKind::page);
      if (!id) return Fail(E::identity_failure);
      pages.push_back(*id);
    }
    for (u64 i = 0; i <= count; ++i) {
      const auto id = issue(UuidKind::object);
      if (!id) return Fail(E::identity_failure);
      allocations.push_back(*id);
    }

    disk::FilespacePageZero zero;
    zero.bootstrap = b;
    zero.page_uuid = *zero_id;
    zero.creation_operation_uuid = request.operation_uuid;
    zero.writer_identity_uuid = request.writer_uuid;
    zero.page_generation = zero.root_set_generation = 1;
    zero.total_pages = total;
    zero.free_pages = total - count - 1;
    zero.creation_utc_millis = request.creation_utc_millis;
    zero.roots.push_back({3, 3, b.filespace_uuid, 1, 1, b.page_size_profile_uuid, *map_id});
    auto zero_image = disk::EncodeFilespacePageZero(zero);
    if (!zero_image.ok()) return Fail(E::encoding_failure);

    std::vector<std::vector<byte>> images(count);
    std::array<byte, 32> successor_hash{};
    for (u64 i = count; i-- > 0;) {
      page::NativeAllocationMap map;
      map.header = {b.page_size_bytes, 3, b.database_uuid, b.filespace_uuid,
                    pages[i], i + 1, 1, 0, b.page_size_profile_uuid};
      map.object_uuid = *map_id;
      map.map_generation = map.capacity_generation = 1;
      map.total_pages = total;
      map.first_page = i * coverage;
      map.creator_transaction_uuid = request.creator.transaction_uuid.value;
      map.creator_local_transaction_id = request.creator.local_id.value;
      const auto covered = std::min(coverage, total - map.first_page);
      map.states.assign(covered, S::free);
      const auto end = std::min(map.first_page + covered, count + 1);
      for (u64 n = map.first_page; n < end; ++n) {
        map.states[n - map.first_page] = S::allocated;
        map.records.push_back(
            {n, allocations[n], n ? pages[n - 1] : *zero_id, n ? *map_id : b.filespace_uuid,
             request.creator.transaction_uuid.value, request.creator.local_id.value,
             1, 0, n ? 3u : (b.filespace_role <= 4 ? 1u : 2u)});
      }
      if (i + 1 < count) {
        map.next = disk::NativePageReference{b.filespace_uuid, i + 2, 1, b.page_size_profile_uuid};
        map.next_sha256 = successor_hash;
      }
      auto encoded = page::EncodeNativeAllocationMap(map);
      if (!encoded.ok()) {
        auto result = Fail(E::encoding_failure);
        result.allocation_error = encoded.error;
        return result;
      }
      const auto digest = core::hash::ComputeSha256Digest(encoded.bytes);
      if (!digest.ok()) return Fail(E::hash_failure);
      successor_hash = digest.digest;
      images[i] = std::move(encoded.bytes);
    }
    const auto zero_hash = core::hash::ComputeSha256Digest(*zero_image.bytes);
    if (!zero_hash.ok()) return Fail(E::hash_failure);
    const NativeFilespaceInitializationReceipt receipt{
        b.database_uuid, b.filespace_uuid, *zero_id, *map_id,
        request.operation_uuid, request.writer_uuid, request.creator, zero.roots.front(),
        zero_hash.digest, successor_hash, total, zero.free_pages, count};

    // No truncate/retry of nonempty bytes. A failed physical attempt belongs
    // to creation recovery; this primitive cannot certify activation.
    std::vector<byte> scratch(size, 0);
    const auto write = [&](u64 number, const auto& bytes) {
      const auto io = device.WriteAt(number * size, bytes.data(), bytes.size());
      return io.ok() && io.bytes_transferred == bytes.size();
    };
    for (u64 n = 0; n < total; ++n)
      if (!write(n, scratch)) return Fail(E::io_failure);
    for (u64 i = 0; i < count; ++i)
      if (!write(i + 1, images[i])) return Fail(E::io_failure);
    if (!write(0, *zero_image.bytes) || !device.Sync().ok()) return Fail(E::io_failure);
    for (u64 n = 0; n < total; ++n) {
      const auto io = device.ReadAt(n * size, scratch.data(), scratch.size());
      if (!io.ok() || io.bytes_transferred != scratch.size()) return Fail(E::io_failure);
      if (n == 0) {
        if (scratch != *zero_image.bytes) return Fail(E::readback_mismatch);
      } else if (n <= count) {
        if (scratch != images[n - 1]) return Fail(E::readback_mismatch);
      } else if (!std::all_of(scratch.begin(), scratch.end(), [](byte value) { return value == 0; })) {
        return Fail(E::readback_mismatch);
      }
    }

    // The retained-image budget covers preparation and this separate actual
    // chain-admission pass; prepared buffers must not remain live across it.
    images.clear();
    zero_image.bytes.reset();
    std::vector<byte>().swap(scratch);
    const auto actual = page::ReadNativeAllocationChainFromOpenDevice(
        device, {b.database_uuid, b.filespace_uuid, b.page_size_profile_uuid}, budget);
    if (!actual.ok()) {
      auto result = Fail(E::allocation_failure);
      result.allocation_error = actual.error;
      return result;
    }
    if (actual.pages.size() != count || actual.state_counts[0] != zero.free_pages ||
        actual.state_counts[2] != count + 1)
      return Fail(E::readback_mismatch);
    NativeFilespaceInitializationResult result;
    result.error = E::none;
    result.receipt = receipt;
    return result;
  } catch (const std::bad_alloc&) {
    return Fail(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Fail(E::resource_exhausted);
  } catch (...) {
    return Fail(E::io_failure);
  }
}
}  // namespace scratchbird::storage::database
