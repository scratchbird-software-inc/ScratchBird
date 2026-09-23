// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_update_durable_frame_store_internal.hpp"
#include "hash_digest.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace api = scratchbird::engine::internal_api;
namespace codec = api::mga_update_durable_detail;
using Identity = api::MgaDmlUpdateDurableOperationIdentityV1;
using Uuid = api::EngineUuid;
using Frame = codec::DmlUpdateDurableFrameV1;
static void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(EXIT_FAILURE); }
}
static constexpr std::array<Uuid Identity::*, 8> members{
    &Identity::database_uuid, &Identity::owning_transaction_uuid,
    &Identity::authenticated_statement_receipt_uuid, &Identity::operation_uuid,
    &Identity::descriptor_uuid, &Identity::recovery_token_uuid,
    &Identity::validated_durable_handle_uuid, &Identity::reserved_statement_barrier_uuid};
static constexpr std::array<std::size_t, 8> offsets{32,48,72,88,112,136,160,184};

static Frame Fixture() {
  Frame f;
  for (unsigned n = 0; n < members.size(); ++n)
    f.identity.*members[n] = scratchbird::tests::FixtureUuid(6013, n + 1);
  f.identity.owning_local_transaction_id = 17;
  f.identity.operation_generation = 1;
  f.identity.descriptor_generation = 2;
  f.identity.recovery_generation = 3;
  f.identity.validated_durable_handle_generation = 4;
  f.identity.reserved_statement_barrier_generation = 5;
  f.sequence = 6;
  f.state = 1;
  f.prior_record_sha256.fill(0x11);
  f.record_evidence_sha256.fill(0x22);
  f.payload = {0,10,9,255,124};
  return f;
}

int main() {
  const auto original = Fixture();
  std::vector<std::uint8_t> golden;
  Check(codec::DmlUpdateDurableEncodeFrame(original, &golden), "valid frame refused");
  // Independent struct/hashlib vector for the pre-existing SBMDUOP1 layout.
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(golden);
  Check(golden.size() == 357 && digest.ok() &&
            scratchbird::core::hash::HexLower(digest.digest) ==
                "1da10d951f971b7607b2712b17979784918eb96bf470467c8db23e48555ee6f4",
        "UPDATE frame wire layout or checksum changed");
  for (unsigned field = 0; field < members.size(); ++field) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto f = original;
      auto& id = f.identity.*members[field];
      id.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      std::vector<std::uint8_t> bytes{42};
      const bool accepted = codec::DmlUpdateDurableEncodeFrame(f, &bytes);
      if ((id.bytes[8] & 0xc0u) != 0x80u) {
        Check(!accepted && bytes == std::vector<std::uint8_t>{42},
              "invalid UUID variant accepted or changed output");
        continue;
      }
      Check(accepted && bytes.size() == golden.size() &&
                std::equal(id.bytes.begin(), id.bytes.end(), bytes.begin() + offsets[field]),
            "UPDATE UUID field is not exact raw16");
      Frame decoded;
      std::string detail;
      Check(codec::DmlUpdateDurableDecodeFrame(bytes, &decoded, &detail) &&
                decoded.identity == f.identity && decoded.payload == f.payload &&
                decoded.sequence == f.sequence && decoded.state == f.state &&
                decoded.prior_record_sha256 == f.prior_record_sha256 &&
                decoded.record_evidence_sha256 == f.record_evidence_sha256,
            "UPDATE frame round trip changed identity or payload");
    }
    auto nil = original;
    nil.identity.*members[field] = {};
    std::vector<std::uint8_t> bytes;
    Check(!codec::DmlUpdateDurableEncodeFrame(nil, &bytes), "required UUID accepted nil");
  }
  for (std::size_t n = 0; n < golden.size(); ++n) {
    Frame decoded;
    std::string detail;
    Check(!codec::DmlUpdateDurableDecodeFrame(
              std::span<const std::uint8_t>(golden).first(n), &decoded, &detail),
          "truncated UPDATE frame accepted");
    auto corrupt = golden;
    corrupt[n] ^= 1;
    Check(!codec::DmlUpdateDurableDecodeFrame(corrupt, &decoded, &detail),
          "corrupt UPDATE frame accepted");
  }
  api::EngineRequestContext context;
  context.database_path = "binary-identity-fixture";
  context.database_uuid = original.identity.database_uuid;
  context.transaction_uuid = original.identity.owning_transaction_uuid;
  context.statement_receipt_uuid = original.identity.authenticated_statement_receipt_uuid;
  context.local_transaction_id = original.identity.owning_local_transaction_id;
  Check(codec::DmlUpdateDurableIdentityMatchesContext(context, original.identity),
        "matching binary context refused");
  for (unsigned field = 0; field < 3; ++field) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = original.identity;
      (changed.*members[field]).bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      Check(!codec::DmlUpdateDurableIdentityMatchesContext(context, changed),
            "different binary context identity matched");
    }
  }
  auto savepoint = original;
  savepoint.kind = codec::DmlUpdateDurableFrameKindV1::statement_savepoint;
  savepoint.identity.validated_durable_handle_uuid = {};
  savepoint.identity.validated_durable_handle_generation = 0;
  savepoint.identity.reserved_statement_barrier_uuid = {};
  savepoint.identity.reserved_statement_barrier_generation = 0;
  std::vector<std::uint8_t> bytes;
  Frame decoded;
  std::string detail;
  Check(codec::DmlUpdateDurableEncodeFrame(savepoint, &bytes) &&
            codec::DmlUpdateDurableDecodeFrame(bytes, &decoded, &detail) &&
            decoded.identity == savepoint.identity,
        "savepoint frame optional nil identities changed");
}
