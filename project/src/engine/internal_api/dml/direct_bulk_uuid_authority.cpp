
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_uuid_authority.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_UUID_IMPLEMENTATION_AUTHORITY
// Allocates stable identities only; UUID sequence is never MGA transaction
// order, visibility, or finality authority.
namespace {

std::uint64_t DirectUuidUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint64_t DirectUuidMix64(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

std::uint64_t DirectUuidReservoirSalt() {
  static const std::uint64_t salt = DirectUuidMix64(
      DirectUuidUnixMillis() ^
      static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(&DirectUuidReservoirSalt)));
  return salt;
}

std::string DirectFormatUuidBytes(const std::array<unsigned char, 16>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      out.push_back('-');
    }
    const auto value = bytes[index];
    out.push_back(kHex[(value >> 4) & 0x0f]);
    out.push_back(kHex[value & 0x0f]);
  }
  return out;
}

std::string DirectFastUuidV7Text(std::uint64_t sequence,
                                 std::uint64_t unix_epoch_millis) {
  const std::uint64_t millis =
      unix_epoch_millis & 0x0000ffffffffffffull;
  const std::uint64_t mixed_a =
      DirectUuidMix64(sequence ^ DirectUuidReservoirSalt());
  const std::uint64_t mixed_b =
      DirectUuidMix64(sequence + 0xd1b54a32d192ed03ull);
  std::array<unsigned char, 16> bytes{};
  bytes[0] = static_cast<unsigned char>((millis >> 40) & 0xffu);
  bytes[1] = static_cast<unsigned char>((millis >> 32) & 0xffu);
  bytes[2] = static_cast<unsigned char>((millis >> 24) & 0xffu);
  bytes[3] = static_cast<unsigned char>((millis >> 16) & 0xffu);
  bytes[4] = static_cast<unsigned char>((millis >> 8) & 0xffu);
  bytes[5] = static_cast<unsigned char>(millis & 0xffu);
  bytes[6] = static_cast<unsigned char>(0x70u | ((mixed_a >> 8) & 0x0fu));
  bytes[7] = static_cast<unsigned char>(mixed_a & 0xffu);
  bytes[8] = static_cast<unsigned char>(0x80u | ((mixed_a >> 56) & 0x3fu));
  bytes[9] = static_cast<unsigned char>((mixed_a >> 48) & 0xffu);
  bytes[10] = static_cast<unsigned char>((mixed_a >> 40) & 0xffu);
  bytes[11] = static_cast<unsigned char>((mixed_b >> 32) & 0xffu);
  bytes[12] = static_cast<unsigned char>((mixed_b >> 24) & 0xffu);
  bytes[13] = static_cast<unsigned char>((mixed_b >> 16) & 0xffu);
  bytes[14] = static_cast<unsigned char>((mixed_b >> 8) & 0xffu);
  bytes[15] = static_cast<unsigned char>(mixed_b & 0xffu);
  return DirectFormatUuidBytes(bytes);
}

class DirectUuidReservoir {
 public:
  struct AcquireStats {
    std::size_t served_from_reservoir = 0;
    std::size_t synchronously_generated = 0;
    bool async_refill_requested = false;
  };

  std::vector<std::string> Acquire(std::size_t count, AcquireStats* stats) {
    std::vector<std::string> out;
    out.reserve(count);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!pool_.empty() && out.size() < count) {
        out.push_back(std::move(pool_.front()));
        pool_.pop_front();
      }
    }
    if (stats != nullptr) {
      stats->served_from_reservoir += out.size();
    }
    const std::size_t missing = count - out.size();
    const std::uint64_t millis = DirectUuidUnixMillis();
    for (std::size_t index = 0; index < missing; ++index) {
      out.push_back(GenerateOne(millis));
    }
    if (stats != nullptr) {
      stats->synchronously_generated += missing;
    }
    const bool refill_requested = MaybeStartRefill();
    if (stats != nullptr) {
      stats->async_refill_requested = refill_requested;
    }
    return out;
  }

 private:
  static constexpr std::size_t kLowWatermark = 131072;
  static constexpr std::size_t kRefillBatch = 262144;
  static constexpr std::size_t kMaxPool = 524288;

  std::string GenerateOne(std::uint64_t unix_epoch_millis) {
    const std::uint64_t sequence =
        next_sequence_.fetch_add(1, std::memory_order_relaxed);
    return DirectFastUuidV7Text(sequence, unix_epoch_millis);
  }

  bool MaybeStartRefill() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pool_.size() >= kLowWatermark) {
        return false;
      }
    }
    bool expected = false;
    if (!refill_running_.compare_exchange_strong(expected,
                                                 true,
                                                 std::memory_order_acq_rel)) {
      return false;
    }
    std::thread([this]() {
      std::vector<std::string> generated;
      generated.reserve(kRefillBatch);
      const std::uint64_t millis = DirectUuidUnixMillis();
      for (std::size_t index = 0; index < kRefillBatch; ++index) {
        generated.push_back(GenerateOne(millis));
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& uuid : generated) {
          if (pool_.size() >= kMaxPool) {
            break;
          }
          pool_.push_back(std::move(uuid));
        }
      }
      refill_running_.store(false, std::memory_order_release);
      (void)MaybeStartRefill();
    }).detach();
    return true;
  }

  std::mutex mutex_;
  std::deque<std::string> pool_;
  std::atomic<std::uint64_t> next_sequence_{
      DirectUuidMix64(DirectUuidReservoirSalt())};
  std::atomic<bool> refill_running_{false};
};

DirectUuidReservoir& DirectBulkUuidReservoir() {
  static auto* reservoir = new DirectUuidReservoir();
  return *reservoir;
}

}  // namespace

DirectBulkUuidBatch BuildDirectBulkUuidBatch(
    const DirectPhysicalBulkAppendRequest& request,
    std::size_t row_count) {
  DirectBulkUuidBatch batch;
  batch.row_uuids.reserve(row_count);
  batch.version_uuids.reserve(row_count);
  if (request.before_row_publication) {
    batch.row_image_uuids.reserve(row_count);
  }
  batch.batch_evidence_id =
      "direct-bulk-uuid-batch:" + request.context.request_id + ":" +
      std::to_string(row_count);
  std::size_t generated_row_count = 0;
  for (std::size_t index = 0; index < row_count; ++index) {
    const bool caller_uuid_available =
        index < request.borrowed_input_rows.size() &&
        !request.borrowed_input_rows[index].requested_row_uuid.canonical.empty();
    if (!caller_uuid_available) {
      ++generated_row_count;
    }
  }
  DirectUuidReservoir::AcquireStats acquire_stats;
  std::vector<std::string> generated_uuids =
      DirectBulkUuidReservoir().Acquire(
          generated_row_count + row_count +
              (request.before_row_publication ? row_count : 0),
                                        &acquire_stats);
  batch.reservoir_served_uuids = acquire_stats.served_from_reservoir;
  batch.reservoir_sync_generated_uuids = acquire_stats.synchronously_generated;
  batch.reservoir_async_refill_requested = acquire_stats.async_refill_requested;
  std::size_t generated_index = 0;
  for (std::size_t index = 0; index < row_count; ++index) {
    const bool caller_uuid_available =
        index < request.borrowed_input_rows.size() &&
        !request.borrowed_input_rows[index].requested_row_uuid.canonical.empty();
    if (!caller_uuid_available) {
      ++batch.generated_row_uuids;
      batch.row_uuids.push_back(std::move(generated_uuids[generated_index++]));
    } else {
      ++batch.caller_row_uuids;
      batch.row_uuids.push_back(
          request.borrowed_input_rows[index].requested_row_uuid.canonical);
    }
    batch.version_uuids.push_back(std::move(generated_uuids[generated_index++]));
    if (request.before_row_publication) {
      batch.row_image_uuids.push_back(
          std::move(generated_uuids[generated_index++]));
    }
  }
  return batch;
}

void AddDirectBulkUuidBatchEvidence(const DirectBulkUuidBatch& batch,
                                    DirectPhysicalBulkAppendResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"direct_bulk_uuid_generation_mode", "batched"});
  result->evidence.push_back({"direct_bulk_uuid_batch", batch.batch_evidence_id});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_capacity",
       std::to_string(batch.row_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_version_capacity",
       std::to_string(batch.version_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_uuid_batch_row_image_capacity",
       std::to_string(batch.row_image_uuids.size())});
  result->evidence.push_back(
      {"direct_bulk_generated_row_uuids",
       std::to_string(batch.generated_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_caller_row_uuids",
       std::to_string(batch.caller_row_uuids)});
  result->evidence.push_back(
      {"direct_bulk_version_uuid_generation_mode", "batched"});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_served",
       std::to_string(batch.reservoir_served_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_sync_generated",
       std::to_string(batch.reservoir_sync_generated_uuids)});
  result->evidence.push_back(
      {"direct_bulk_uuid_reservoir_async_refill_requested",
       batch.reservoir_async_refill_requested ? "true" : "false"});
  result->evidence.push_back(
      {"orh_210_batched_uuid_generation", "row_and_version_batch"});
}

}  // namespace scratchbird::engine::internal_api::dml::detail
