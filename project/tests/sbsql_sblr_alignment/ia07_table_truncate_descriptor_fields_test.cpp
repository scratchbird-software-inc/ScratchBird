// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Every field of the issued TRUNCATE carrier, asserted against the
// specification table rather than against the encoder.
//
// SBSQL-TRUNCATE-TABLE-V1 section 6.1 (SBLR_TABLE_TRUNCATE_CANONICAL_FIELDS_V1)
// fixes TTRD424's canonical body at absolute wire offsets 16..383. The encoder
// validates that body, so a descriptor that merely encodes proves only that the
// encoder was satisfied. This gate reads each offset independently and checks
// the value the specification requires there, and recomputes the evidence
// digest from the documented domain string rather than trusting the codec.

#include "../support/binary_uuid_fixture.hpp"
#include "engine/internal_api/sblr_table_truncate_coordinator.hpp"
#include "hash_digest.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace a = scratchbird::engine::internal_api;

int checks = 0;
void Check(bool pass, std::string_view what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL truncate descriptor fields: %.*s\n",
                 static_cast<int>(what.size()), what.data());
    std::exit(EXIT_FAILURE);
  }
}

// The body starts at wire offset 16, so body index = offset - 16.
constexpr std::size_t kBase = 16;
std::uint64_t Field(const std::array<std::uint8_t, 368>& body,
                    std::size_t offset, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value |= static_cast<std::uint64_t>(body[offset - kBase + i]) << (8 * i);
  return value;
}
std::array<std::uint8_t, 16> Uuid(const std::array<std::uint8_t, 368>& body,
                                  std::size_t offset) {
  std::array<std::uint8_t, 16> out{};
  for (unsigned i = 0; i < 16; ++i) out[i] = body[offset - kBase + i];
  return out;
}
std::array<std::uint8_t, 32> Sha(const std::array<std::uint8_t, 368>& body,
                                 std::size_t offset) {
  std::array<std::uint8_t, 32> out{};
  for (unsigned i = 0; i < 32; ++i) out[i] = body[offset - kBase + i];
  return out;
}
bool Nonzero(const std::array<std::uint8_t, 32>& value) {
  for (const auto byte : value) if (byte != 0) return true;
  return false;
}
// Section 6.1: every UUID in these carriers is an exact binary16 RFC-variant
// UUIDv7. Checked from the bytes, not by calling the engine's predicate.
bool SystemUuid(const std::array<std::uint8_t, 16>& value) {
  bool any = false;
  for (const auto byte : value) if (byte != 0) any = true;
  return any && (value[6] >> 4) == 7 && (value[8] & 0xc0) == 0x80;
}
std::array<std::uint8_t, 16> Bytes(const a::EngineUuid& value) {
  return value.bytes;
}

a::EngineRequestContext Context() {
  a::EngineRequestContext c;
  c.security_context_present = true;
  c.statement_metadata_snapshot_engine_owned = true;
  c.database_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a0");
  c.session_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a1");
  c.transaction_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a2");
  c.statement_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a3");
  c.statement_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a4");
  c.catalog_generation_id = 41;
  c.security_epoch = 17;
  c.trace_tags = {"private_table_truncate_compiler"};
  return c;
}

// FixtureUuidLiteral is consteval over a char array, so each distinct target
// is its own literal rather than a composed string.
a::SblrTableTruncateResolvedTargetV1 Target() {
  a::SblrTableTruncateResolvedTargetV1 target;
  target.persisted_descriptor_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024a5");
  target.persisted_descriptor_generation = 9;
  return target;
}
a::SblrTableTruncateResolvedTargetV1 OtherTable() {
  a::SblrTableTruncateResolvedTargetV1 target;
  target.persisted_descriptor_uuid =
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-0000000024b7");
  target.persisted_descriptor_generation = 9;
  return target;
}
a::SblrTableTruncateResolvedTargetV1 OtherGeneration() {
  auto target = Target();
  target.persisted_descriptor_generation = 11;
  return target;
}
}  // namespace

int main() {
  const auto context = Context();
  constexpr std::uint64_t kStructural = 880;
  constexpr std::uint32_t kTruncate = 27;
  constexpr std::uint64_t kAvailability = 5;

  const auto issued = a::CompileSblrTableTruncateDescriptor(
      context, context.statement_uuid, kStructural, kTruncate, kAvailability,
      Target());
  if (!issued.ok) {
    std::fprintf(stderr, "compile refused: %s:%s\n",
                 issued.diagnostic.code.c_str(),
                 issued.diagnostic.message_key.c_str());
  }
  Check(issued.ok, "a complete issuance was refused");
  const auto& body = issued.descriptor.canonical_body;

  // The authenticated tuple, offsets 16..96.
  Check(Uuid(body, 16) == Bytes(context.statement_uuid),
        "offset 16 is not the preliminary receipt");
  Check(Uuid(body, 32) == Bytes(context.database_uuid),
        "offset 32 is not the database/node identity");
  Check(Uuid(body, 48) == Bytes(context.session_uuid),
        "offset 48 is not the session identity");
  Check(Uuid(body, 64) == Bytes(context.transaction_uuid),
        "offset 64 is not the transaction identity");
  Check(Uuid(body, 80) == Bytes(context.statement_uuid),
        "offset 80 is not the statement identity");
  Check(Uuid(body, 96) == Bytes(context.statement_snapshot_uuid),
        "offset 96 is not the statement snapshot identity");

  // The engine-owned descriptor identity: issued here, not supplied.
  Check(SystemUuid(Uuid(body, 112)),
        "offset 112 is not an engine identity UUIDv7");
  Check(Uuid(body, 112) != Bytes(context.statement_uuid) &&
            Uuid(body, 112) != Bytes(context.transaction_uuid),
        "offset 112 reused an authenticated identity");

  // Counters and occurrences.
  Check(Field(body, 128, 8) == 1, "offset 128 is not descriptor generation 1");
  Check(Field(body, 136, 8) == kStructural,
        "offset 136 is not the admitted structural occurrence");
  Check(Field(body, 144, 4) == kTruncate,
        "offset 144 is not the admitted truncate occurrence");

  // Section 2 defaults: CONTINUE IDENTITY, RESTRICT, local scope.
  Check(Field(body, 148, 1) == 1, "offset 148 is not CONTINUE IDENTITY");
  Check(Field(body, 149, 1) == 1, "offset 149 is not RESTRICT");
  Check(Field(body, 150, 2) == 0, "offset 150 set a flag beyond local scope");
  Check(Field(body, 152, 4) == 1, "offset 152 is not one explicit target");
  Check(Field(body, 156, 4) == 1, "offset 156 is not one final target");
  Check(Field(body, 152, 4) <= Field(body, 156, 4),
        "explicit target count exceeds the final count");

  // Closure counts: zero for a single target with no FK or partition edges,
  // and zero identity resets, which section 6.1 requires for CONTINUE.
  Check(Field(body, 160, 4) == 0, "offset 160 claimed FK edges");
  Check(Field(body, 164, 4) == 0, "offset 164 claimed partition edges");
  Check(Field(body, 168, 4) == 0, "offset 168 claimed trigger effects");
  Check(Field(body, 172, 4) == 0,
        "offset 172 claimed identity resets under CONTINUE IDENTITY");

  // Captured epochs come from the receipt context, not from defaults.
  Check(Field(body, 176, 8) == context.catalog_generation_id,
        "offset 176 is not the captured catalog epoch");
  Check(Field(body, 184, 8) == context.security_epoch,
        "offset 184 is not the captured policy epoch");
  Check(Field(body, 192, 8) == 0,
        "offset 192 set a route epoch for a local-scope descriptor");
  Check(Field(body, 200, 8) == 0,
        "offset 200 is not the outer transaction scope");

  // Proofs and intents: four distinct engine identities.
  const std::array<std::size_t, 4> proof_offsets{272, 288, 304, 320};
  for (const auto offset : proof_offsets) {
    Check(SystemUuid(Uuid(body, offset)),
          "a proof or intent identity is not an engine identity UUIDv7");
  }
  for (std::size_t i = 0; i < proof_offsets.size(); ++i) {
    for (std::size_t j = i + 1; j < proof_offsets.size(); ++j) {
      Check(Uuid(body, proof_offsets[i]) != Uuid(body, proof_offsets[j]),
            "two proof or intent identities are the same value");
    }
    Check(Uuid(body, proof_offsets[i]) != Uuid(body, 112),
          "a proof identity reused the descriptor identity");
  }

  // Three digests, all non-zero and all different from each other.
  const auto closure = Sha(body, 208);
  const auto frozen = Sha(body, 240);
  const auto semantic = Sha(body, 336);
  Check(Nonzero(closure), "offset 208 closure digest is zero");
  Check(Nonzero(frozen), "offset 240 frozen payload digest is zero");
  Check(Nonzero(semantic), "offset 336 semantic digest is zero");
  Check(closure != frozen && closure != semantic && frozen != semantic,
        "two digests are identical, so one was copied rather than computed");

  // Reserved bytes.
  for (std::size_t offset = 368; offset < 384; ++offset) {
    Check(body[offset - kBase] == 0, "a reserved byte at 368..383 is not zero");
  }

  Check(issued.descriptor.availability_generation == kAvailability,
        "the availability generation was not carried");

  // Evidence recomputed from the documented domain string. The codec computes
  // it too; this proves the stored value is the specified function of the body
  // rather than whatever the codec happened to write.
  {
    constexpr std::string_view kDomain =
        "ScratchBird.SblrTableTruncateDescriptor.V1";
    std::vector<std::uint8_t> material(kDomain.begin(), kDomain.end());
    material.insert(material.end(), body.begin(), body.end());
    const auto hashed = scratchbird::core::hash::ComputeSha256Digest(material);
    Check(hashed.ok(), "independent evidence digest failed");
    Check(hashed.digest == issued.descriptor.evidence,
          "the carried evidence is not SHA256(domain || body 16..383)");
  }

  // The closure digest binds the resolved target: a different target must
  // change it, and must change the frozen record digest with it.
  {
    const auto other = a::CompileSblrTableTruncateDescriptor(
        context, context.statement_uuid, kStructural, kTruncate, kAvailability,
        OtherTable());
    Check(other.ok, "a second issuance with another target was refused");
    Check(Sha(other.descriptor.canonical_body, 208) != closure,
          "a different target produced the same closure digest");
    Check(Sha(other.descriptor.canonical_body, 240) != frozen,
          "a different target produced the same frozen record digest");
  }
  // And the generation is part of it too.
  {
    const auto other = a::CompileSblrTableTruncateDescriptor(
        context, context.statement_uuid, kStructural, kTruncate, kAvailability,
        OtherGeneration());
    Check(other.ok, "a second issuance with another generation was refused");
    Check(Sha(other.descriptor.canonical_body, 208) != closure,
          "a different target generation produced the same closure digest");
  }

  // Each issuance mints its own descriptor identity and proofs.
  {
    const auto again = a::CompileSblrTableTruncateDescriptor(
        context, context.statement_uuid, kStructural, kTruncate, kAvailability,
        Target());
    Check(again.ok, "a repeat issuance was refused");
    Check(Uuid(again.descriptor.canonical_body, 112) != Uuid(body, 112),
          "two issuances shared one descriptor identity");
    Check(again.descriptor.evidence != issued.descriptor.evidence,
          "two issuances produced the same evidence digest");
  }

  std::printf("PASS truncate descriptor fields checks=%d\n", checks);
  return 0;
}
