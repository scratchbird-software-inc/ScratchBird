// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "overflow_persistence.hpp"
#include "page_header.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::storage::page::BuildOverflowChunkPageBody;
using scratchbird::storage::page::CommitOverflowValue;
using scratchbird::storage::page::FindOverflowValue;
using scratchbird::storage::page::OverflowCommitRequest;
using scratchbird::storage::page::OverflowLedger;
using scratchbird::storage::page::OverflowPersistRequest;
using scratchbird::storage::page::ParseOverflowChunkPageBody;
using scratchbird::storage::page::PersistOverflowValue;
using scratchbird::storage::page::ReadOverflowValue;
using scratchbird::storage::page::ValidateOverflowChunkPageBody;

TypedUuid MakeUuid(UuidKind kind, u64 seed) {
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(kind, 1832000000000ull + seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::vector<byte> Payload(std::size_t bytes) {
  std::vector<byte> payload;
  payload.reserve(bytes);
  for (std::size_t index = 0; index < bytes; ++index) {
    payload.push_back(static_cast<byte>((index * 17 + 3) & 0xffu));
  }
  return payload;
}

bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::cerr << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  constexpr u32 kPageSize = 8192;
  constexpr u32 kChunkSize = 64;
  constexpr u64 kGeneration = 7;
  constexpr u64 kLocalTransactionId = 42;
  constexpr std::size_t kPayloadBytes = 211;
  constexpr std::size_t kBodyPayloadOffset = 176;

  OverflowLedger ledger;
  OverflowPersistRequest request;
  request.row_uuid = MakeUuid(UuidKind::row, 1);
  request.object_uuid = MakeUuid(UuidKind::object, 2);
  request.transaction_uuid = MakeUuid(UuidKind::transaction, 3);
  request.chunk_policy_uuid = MakeUuid(UuidKind::object, 4);
  request.local_transaction_id = kLocalTransactionId;
  request.generation = kGeneration;
  request.value_descriptor = "scratchbird.public.overflow.binary_descriptor.fixture";
  request.payload_bytes = Payload(kPayloadBytes);
  request.chunk_size = kChunkSize;

  const auto persisted = PersistOverflowValue(&ledger, request);
  ok = Expect(persisted.ok(), "overflow value did not persist") && ok;
  ok = Expect(persisted.chunk_count == 4, "overflow chunk count was not deterministic") && ok;

  OverflowCommitRequest commit;
  commit.overflow_value_uuid = persisted.overflow_value_uuid;
  commit.transaction_uuid = request.transaction_uuid;
  commit.local_transaction_id = request.local_transaction_id;
  commit.reason = "PCR-032 binary page body commit";
  const auto committed = CommitOverflowValue(&ledger, commit);
  ok = Expect(committed.ok(), "overflow value did not commit") && ok;

  const auto* record = FindOverflowValue(ledger, persisted.overflow_value_uuid);
  ok = Expect(record != nullptr, "committed overflow record was not found") && ok;
  if (record == nullptr) {
    return 1;
  }

  std::size_t expected_offset = 0;
  for (const auto& chunk : record->chunks) {
    const auto built = BuildOverflowChunkPageBody(*record, chunk, kPageSize);
    ok = Expect(built.ok(), "overflow chunk page body did not build") && ok;
    ok = Expect(built.serialized.size() ==
                    kPageSize - scratchbird::storage::disk::kPageHeaderSerializedBytes,
                "overflow chunk body does not occupy page body capacity") &&
         ok;

    const auto parsed = ParseOverflowChunkPageBody(built.serialized);
    ok = Expect(parsed.ok(), "overflow chunk page body did not parse") && ok;
    if (parsed.ok()) {
      const auto validated = ValidateOverflowChunkPageBody(*record, chunk, parsed.body);
      ok = Expect(validated.ok(), "overflow chunk page body did not validate") && ok;
      ok = Expect(parsed.body.generation == kGeneration,
                  "overflow chunk page body lost generation") &&
           ok;
      ok = Expect(parsed.body.local_transaction_id == kLocalTransactionId,
                  "overflow chunk page body lost local transaction id") &&
           ok;
      ok = Expect(parsed.body.byte_offset == expected_offset,
                  "overflow chunk page body byte offset drifted") &&
           ok;
      ok = Expect(parsed.body.content_hash == record->content_hash,
                  "overflow chunk page body content hash drifted") &&
           ok;
    }

    auto corrupt = built.serialized;
    corrupt[kBodyPayloadOffset + record->content_hash.size()] ^= 0x7fu;
    const auto corrupted = ParseOverflowChunkPageBody(corrupt);
    ok = Expect(!corrupted.ok(),
                "overflow chunk page body accepted corrupted payload bytes") &&
         ok;

    expected_offset += chunk.byte_count;
  }

  scratchbird::storage::page::OverflowReadRequest read;
  read.overflow_value_uuid = persisted.overflow_value_uuid;
  const auto read_result = ReadOverflowValue(ledger, read);
  ok = Expect(read_result.ok(), "committed overflow value did not read") && ok;
  ok = Expect(read_result.payload_bytes == request.payload_bytes,
              "overflow read did not reconstruct original payload") &&
       ok;

  if (!ok) {
    return 1;
  }
  std::cout << "public_toast_overflow_binary_descriptor_gate=passed\n";
  return 0;
}
