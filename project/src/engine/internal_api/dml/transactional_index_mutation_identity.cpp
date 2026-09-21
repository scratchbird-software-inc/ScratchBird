// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/transactional_index_provider.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <limits>
#include <vector>

namespace scratchbird::engine::internal_api {
// IMT-MUTATION-EVIDENCE-BINARY-002. Evidence only; inventory and base versions
// still determine visibility, ownership, successful publication and finality.
std::string DmlTransactionalIndexMutationIdentity(
    const EngineRequestContext& context,
    const DmlTransactionalIndexEntryRequest& request,
    std::string_view mutation_kind) {
  using scratchbird::core::uuid::IsEngineIdentityUuid;
  const std::uint8_t kind = mutation_kind == "insert" ? 1 :
      mutation_kind == "retire" ? 2 : mutation_kind == "rebuild" ? 3 : 0;
  if (!kind || !context.local_transaction_id || !request.index.event_sequence ||
      !IsEngineIdentityUuid(context.database_uuid) ||
      !IsEngineIdentityUuid(context.transaction_uuid) ||
      !IsEngineIdentityUuid(request.index.index_uuid) ||
      !IsEngineIdentityUuid(request.table_uuid) ||
      request.index.table_uuid != request.table_uuid ||
      !IsEngineIdentityUuid(request.row_uuid) ||
      !IsEngineIdentityUuid(request.version_uuid) ||
      ((kind == 2 || !request.predecessor_version_uuid.is_nil()) &&
       !IsEngineIdentityUuid(request.predecessor_version_uuid))) return {};

  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  constexpr std::string_view domain = "SB_DML_TRANSACTIONAL_INDEX_MUTATION_V2";
  constexpr std::size_t fixed = domain.size() + 7 * 16 + 4 * 8 + 1;
  const auto max = std::numeric_limits<std::size_t>::max();
  if (request.key_value.size() > max - fixed ||
      request.payload_value.size() > max - fixed - request.key_value.size()) return {};
  std::vector<scratchbird::core::platform::byte> bytes;
  bytes.reserve(fixed + request.key_value.size() + request.payload_value.size());
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  const auto uuid = [&](const EngineUuid& id) {
    bytes.insert(bytes.end(), id.bytes.begin(), id.bytes.end());
  };
  const auto u64 = [&](std::uint64_t n) {
    for (unsigned shift = 0; shift < 64; shift += 8)
      bytes.push_back(static_cast<std::uint8_t>(n >> shift));
  };
  const auto value = [&](const std::string& v) {
    u64(v.size()); bytes.insert(bytes.end(), v.begin(), v.end());
  };
  uuid(context.database_uuid); uuid(context.transaction_uuid);
  u64(context.local_transaction_id); uuid(request.index.index_uuid);
  u64(request.index.event_sequence); uuid(request.table_uuid);
  uuid(request.row_uuid); uuid(request.version_uuid);
  uuid(request.predecessor_version_uuid); bytes.push_back(kind);
  value(request.key_value); value(request.payload_value);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
  return digest.ok() ? scratchbird::core::hash::HexLower(digest.digest) : std::string{};
}
}  // namespace scratchbird::engine::internal_api
