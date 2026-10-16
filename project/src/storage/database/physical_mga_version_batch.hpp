// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "uuid.hpp"
#include "blake3_digest.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace scratchbird::engine::internal_api::dml {
struct DirectPhysicalBulkAppendRequest;
namespace detail {
struct DirectBulkUuidBatch;
DirectBulkUuidBatch BuildDirectBulkUuidBatch(const DirectPhysicalBulkAppendRequest&, std::size_t);
}
}

namespace scratchbird::storage::database {
// Lexical provenance for versions already issued by the native bulk UUID
// issuer. NOT catalog, memory, security, MGA admission, completion or finality.
// Default construction is unbound; only the actual issuer can seal a batch.
// Borrows its issuer's stable binary arrays through the synchronous physical
// writer join. Quiesce all users before move/destruction/array mutation.
// The private digest authenticates the exact binary scope and complete array
// contents; a caller cannot modify the issuer's public projections to mint a
// different physical identity. One atomic claim prevents duplicate execution
// through copied requests. A failed attempt never rearms the batch.
class PhysicalMgaCowVersionBatch final {
 public:
  PhysicalMgaCowVersionBatch() noexcept = default;
  PhysicalMgaCowVersionBatch(const PhysicalMgaCowVersionBatch&) = delete;
  PhysicalMgaCowVersionBatch& operator=(const PhysicalMgaCowVersionBatch&) = delete;
  PhysicalMgaCowVersionBatch(PhysicalMgaCowVersionBatch&&) noexcept;
  PhysicalMgaCowVersionBatch& operator=(PhysicalMgaCowVersionBatch&&) noexcept;
 private:
  using Uuid = core::platform::Uuid;
  PhysicalMgaCowVersionBatch(const Uuid&, const Uuid&, const Uuid&, std::uint64_t,
      std::span<const Uuid>, std::span<const Uuid>);
  bool Digest(std::array<std::uint8_t,32>&) const noexcept;
  Uuid database_, relation_, transaction_;
  std::uint64_t local_id_ = 0;
  std::span<const Uuid> rows_, versions_;
  std::array<std::uint8_t,32> digest_{};
  std::atomic<bool> claimed_{true};
  friend class PhysicalMgaCowVersionBatchAccess;
  friend engine::internal_api::dml::detail::DirectBulkUuidBatch
      engine::internal_api::dml::detail::BuildDirectBulkUuidBatch(
          const engine::internal_api::dml::DirectPhysicalBulkAppendRequest&, std::size_t);
};
inline bool PhysicalMgaCowVersionBatch::Digest(std::array<std::uint8_t,32>& out) const noexcept {
  namespace hash = core::hash;
  if (rows_.size() != versions_.size() ||
      rows_.size() > (UINT64_MAX - 128) / 32) return false;
  hash::Blake3IncrementalStateV3 state;
  struct Scrub { hash::Blake3IncrementalStateV3& s; ~Scrub() { hash::ScrubBlake3IncrementalV3(&s); } } scrub{state};
  using S = hash::Blake3IncrementalStatusV3;
  const auto append = [&](const void* data, std::size_t size) {
    return hash::UpdateBlake3IncrementalV3(&state,
        static_cast<const std::uint8_t*>(data), size) == S::kOk;
  };
  const auto number = [&](std::uint64_t value) {
    std::array<std::uint8_t,8> bytes{};
    for (unsigned i=0; i<8; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (8*i));
    return append(bytes.data(), bytes.size());
  };
  constexpr char domain[] = "ScratchBird/native-bulk/issued-row-version-batch/v1";
  if (hash::InitializeBlake3IncrementalV3(&state) != S::kOk ||
      !append(domain, sizeof(domain)) || !append(database_.bytes.data(),16) ||
      !append(relation_.bytes.data(),16) || !append(transaction_.bytes.data(),16) ||
      !number(local_id_) || !number(rows_.size())) return false;
  for (std::size_t i=0; i<rows_.size(); ++i)
    if (!append(rows_[i].bytes.data(),16) || !append(versions_[i].bytes.data(),16)) return false;
  return hash::FinalizeBlake3IncrementalV3(&state, &out) == S::kOk;
}

inline PhysicalMgaCowVersionBatch::PhysicalMgaCowVersionBatch(
    const Uuid& database, const Uuid& relation, const Uuid& transaction,
    std::uint64_t local_id, std::span<const Uuid> rows, std::span<const Uuid> versions)
    : database_(database), relation_(relation), transaction_(transaction),
      local_id_(local_id), rows_(rows), versions_(versions) {
  claimed_.store(!Digest(digest_), std::memory_order_relaxed);
}
inline PhysicalMgaCowVersionBatch::PhysicalMgaCowVersionBatch(PhysicalMgaCowVersionBatch&& other) noexcept {
  *this = std::move(other);
}
inline PhysicalMgaCowVersionBatch& PhysicalMgaCowVersionBatch::operator=(PhysicalMgaCowVersionBatch&& other) noexcept {
  if (this != &other) {
    database_=other.database_; relation_=other.relation_; transaction_=other.transaction_;
    local_id_=other.local_id_; digest_=other.digest_;
    rows_=std::exchange(other.rows_,{}); versions_=std::exchange(other.versions_,{});
    claimed_.store(other.claimed_.exchange(true,std::memory_order_acq_rel),std::memory_order_release);
  }
  return *this;
}

} // namespace scratchbird::storage::database
