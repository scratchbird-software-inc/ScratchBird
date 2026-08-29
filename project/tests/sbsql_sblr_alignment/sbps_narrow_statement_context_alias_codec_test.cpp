// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "wire/parser_server_ipc/sbps_narrow_statement_context_alias_codec.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ipc = scratchbird::parser::ipc;
using scratchbird::core::platform::byte;

static_assert(ipc::kPsNarrowStatementContextRequestMessageV1 == 696);
static_assert(ipc::kPsNarrowStatementContextRequestSchemaV1 == 7709);
static_assert(ipc::kPsNarrowStatementContextResultMessageV1 == 697);
static_assert(ipc::kPsNarrowStatementContextResultSchemaV1 == 7710);
static_assert(ipc::kPsStatementContextSourceRequestSchemaV11 == 7031);
static_assert(ipc::kPsStatementContextSourceResultSchemaV11 == 7032);

void Require(bool condition, const std::string& detail) {
  if (!condition) throw std::runtime_error(detail);
}

ipc::PsStatementContextUuidV1 Uuid(std::uint16_t discriminator) {
  ipc::PsStatementContextUuidV1 uuid{};
  uuid[0] = 0x01;
  uuid[1] = 0x9d;
  uuid[6] = 0x70;
  uuid[8] = 0x80;
  uuid[14] = static_cast<byte>((discriminator >> 8u) & 0xffu);
  uuid[15] = static_cast<byte>(discriminator & 0xffu);
  return uuid;
}

void U16(std::vector<byte>* out, std::uint16_t value) {
  out->push_back(static_cast<byte>(value & 0xffu));
  out->push_back(static_cast<byte>((value >> 8u) & 0xffu));
}

void U32(std::vector<byte>* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<byte>((value >> shift) & 0xffu));
  }
}

void U64(std::vector<byte>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<byte>((value >> shift) & 0xffu));
  }
}

void PutUuid(std::vector<byte>* out,
             const ipc::PsStatementContextUuidV1& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void PutString(std::vector<byte>* out, std::string_view value) {
  U16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void StoreU16(std::vector<byte>* out, std::size_t offset,
              std::uint16_t value) {
  (*out)[offset] = static_cast<byte>(value & 0xffu);
  (*out)[offset + 1] = static_cast<byte>((value >> 8u) & 0xffu);
}

void StoreU64(std::vector<byte>* out, std::size_t offset,
              std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    (*out)[offset + shift / 8] =
        static_cast<byte>((value >> shift) & 0xffu);
  }
}

ipc::PsStatementContextUuidV1 TypeForKind(std::uint8_t kind) {
  switch (kind) {
    case 11:
    case 18:
    case 19:
      return Uuid(301);
    case 12:
    case 14:
    case 15:
      return Uuid(302);
    case 13:
    case 16:
    case 17:
      return Uuid(303);
    case 20:
    case 21:
      return Uuid(304);
    case 22:
    case 23:
      return Uuid(305);
    default:
      return Uuid(static_cast<std::uint16_t>(400 + kind));
  }
}

struct ResultFixture {
  std::vector<byte> bytes;
  std::size_t extension = 0;
  std::size_t trailer = 0;
};

ResultFixture Schema7032V71Fixture() {
  ResultFixture fixture;
  auto& out = fixture.bytes;
  U16(&out, 11);
  out.push_back(1);
  PutUuid(&out, Uuid(1));
  U64(&out, 42);
  PutUuid(&out, Uuid(2));
  PutUuid(&out, Uuid(3));
  PutUuid(&out, Uuid(4));
  PutUuid(&out, Uuid(5));
  PutUuid(&out, Uuid(6));
  U64(&out, 7);
  Require(out.size() == 115, "schema7032 base prefix fixture drifted");
  PutString(&out, "2026-08-27T12:34:56Z");
  for (std::uint16_t index = 0; index < 6; ++index) {
    PutUuid(&out, Uuid(static_cast<std::uint16_t>(10 + index)));
  }

  U16(&out, 43);
  for (std::uint16_t index = 0; index < 43; ++index) {
    U16(&out, 1);
    PutString(&out, "sb.aggregate.fn" + std::to_string(index));
    PutUuid(&out, Uuid(static_cast<std::uint16_t>(1000 + index)));
    out.push_back(1);
  }
  U16(&out, 11);
  for (std::uint16_t index = 0; index < 11; ++index) {
    U16(&out, 1);
    PutString(&out, "sb.window.fn" + std::to_string(index));
    PutUuid(&out, Uuid(static_cast<std::uint16_t>(1100 + index)));
    out.push_back(1);
  }

  U16(&out, 646);
  for (std::uint16_t index = 0; index < 646; ++index) {
    std::uint8_t kind = 0;
    std::uint16_t slot = 0;
    if (index < 320) {
      kind = static_cast<std::uint8_t>(index / 32 + 1);
      slot = static_cast<std::uint16_t>(index % 32);
    } else if (index < 322) {
      kind = 11;
      slot = static_cast<std::uint16_t>(index - 320);
    } else if (index < 324) {
      kind = 12;
      slot = static_cast<std::uint16_t>(index - 322);
    } else if (index < 326) {
      kind = 13;
      slot = static_cast<std::uint16_t>(index - 324);
    } else {
      kind = static_cast<std::uint8_t>(14 + (index - 326) / 32);
      slot = static_cast<std::uint16_t>((index - 326) % 32);
    }
    out.push_back(kind);
    U16(&out, slot);
    PutUuid(&out, Uuid(static_cast<std::uint16_t>(2000 + index)));
    PutUuid(&out, TypeForKind(kind));
    PutUuid(&out, {});
    const bool nullable =
        (kind <= 10 && kind % 2 == 0) || (kind >= 14 && kind % 2 == 1);
    out.push_back(nullable ? 1 : 0);
    U32(&out, 0);
    U32(&out, 0);
    U32(&out, 0);
  }

  fixture.extension = out.size();
  U16(&out, 71);
  U16(&out, 0);
  PutUuid(&out, Uuid(20));
  PutUuid(&out, Uuid(21));
  U64(&out, 1);
  U64(&out, 2);
  U64(&out, 3);
  PutUuid(&out, Uuid(3));
  PutUuid(&out, {});
  U64(&out, 0);
  PutUuid(&out, {});
  U64(&out, 0);
  PutUuid(&out, {});
  U64(&out, 0);
  U64(&out, 4);
  PutUuid(&out, {});
  U64(&out, 0);
  PutUuid(&out, {});
  U64(&out, 0);
  PutUuid(&out, {});
  U64(&out, 0);
  PutUuid(&out, Uuid(22));
  U64(&out, 5);
  U32(&out, 1);
  U32(&out, 72);
  Require(out.size() == fixture.extension + 260,
          "schema7032 v71 extension prefix fixture drifted");
  PutUuid(&out, Uuid(23));
  U64(&out, 6);
  U32(&out, 0);
  out.push_back(1);
  out.push_back(1);
  U16(&out, 0);
  U32(&out, 4);
  U32(&out, 0);
  out.insert(out.end(), 32, 0x5a);
  fixture.trailer = out.size();

  PutUuid(&out, Uuid(24));
  U64(&out, 7);
  PutUuid(&out, Uuid(25));
  U64(&out, 8);
  U64(&out, 9);
  U64(&out, 0);
  out.push_back(1);
  out.push_back(1);
  out.push_back(1);
  out.insert(out.end(), 5, 0);
  U64(&out, 0);
  U64(&out, 10);
  out.push_back(1);
  out.push_back(1);
  out.push_back(1);
  out.insert(out.end(), 5, 0);
  U64(&out, 0);
  U64(&out, 11);
  out.push_back(1);
  out.push_back(1);
  out.push_back(1);
  out.insert(out.end(), 5, 0);
  U64(&out, 0);
  for (std::uint64_t generation = 1; generation <= 61; ++generation) {
    U64(&out, generation);
  }
  Require(out.size() == fixture.trailer + 616,
          "schema7032 v71 executor vector fixture drifted");
  out.insert(out.end(), {'T', 'X', 'B', 'H'});
  U16(&out, 1);
  U16(&out, 152);
  U32(&out, 152);
  U32(&out, 0);
  PutUuid(&out, Uuid(2));
  U64(&out, 42);
  PutUuid(&out, Uuid(26));
  PutUuid(&out, Uuid(24));
  U64(&out, 7);
  PutUuid(&out, Uuid(25));
  U64(&out, 8);
  out.push_back(1);
  out.push_back(1);
  out.push_back(1);
  out.insert(out.end(), 5, 0);
  out.insert(out.end(), 32, 0xa5);
  U64(&out, 9);
  Require(out.size() == fixture.trailer + 768,
          "schema7032 v71 TXBH fixture drifted");
  U64(&out, 65536);
  Require(out.size() == fixture.trailer + 776,
          "schema7032 v71 trailer fixture drifted");
  return fixture;
}

void RequestAliasContract() {
  ipc::PsNarrowStatementContextRequestValidationContextV1 context;
  context.expected_session_uuid = Uuid(1);
  context.expected_owning_local_transaction_id = 42;
  context.expected_owning_transaction_uuid = Uuid(2);
  ipc::PsNarrowStatementContextRequestV1 request;
  request.session_uuid = Uuid(1);
  request.owning_local_transaction_id = 42;
  request.owning_transaction_uuid = Uuid(2);
  const auto encoded =
      ipc::EncodeAndValidatePsNarrowStatementContextRequestV1(request, context);
  Require(encoded.ok() && encoded.canonical_payload.size() == 42,
          "7709 did not emit exact schema7031-v11 bytes");
  const auto alias =
      ipc::ValidatePsNarrowStatementContextRequestAliasIdentityV1(
          encoded.canonical_payload, encoded.canonical_payload, context);
  Require(alias.ok() && alias.canonical_payload == encoded.canonical_payload,
          "7709 byte identity failed");

  auto drifted = encoded.canonical_payload;
  drifted.back() ^= 1;
  const auto refused =
      ipc::ValidatePsNarrowStatementContextRequestAliasIdentityV1(
          drifted, encoded.canonical_payload, context);
  Require(!refused.ok() && refused.canonical_payload.empty(),
          "7709 admitted source-byte drift");
}

void ResultAliasContract() {
  const auto fixture = Schema7032V71Fixture();
  const auto accepted =
      ipc::ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
          fixture.bytes, fixture.bytes);
  Require(accepted.ok() && accepted.canonical_payload == fixture.bytes,
          "7710 did not retain byte-identical schema7032-v71 payload");
  Require(accepted.summary.extension_offset == fixture.extension &&
              accepted.summary.diagnostic_identity_row_count == 1 &&
              accepted.summary.maximum_mga_relation_decoded_bytes_per_pass ==
                  65536 &&
              accepted.summary.owning_local_transaction_id == 42,
          "7710 summary drifted");

  auto alias_drift = fixture.bytes;
  alias_drift.back() ^= 1;
  const auto mismatched =
      ipc::ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
          alias_drift, fixture.bytes);
  Require(!mismatched.ok() && mismatched.canonical_payload.empty(),
          "7710 admitted non-identical alias bytes");

  auto version70 = fixture.bytes;
  StoreU16(&version70, fixture.extension, 70);
  const auto old_version =
      ipc::ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
          version70, version70);
  Require(!old_version.ok() && old_version.canonical_payload.empty(),
          "7710 admitted schema7032 extension v70");

  auto zero_create_index_generation = fixture.bytes;
  StoreU64(&zero_create_index_generation, fixture.trailer + 600, 0);
  const auto missing_create_index_generation =
      ipc::ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
          zero_create_index_generation, zero_create_index_generation);
  Require(!missing_create_index_generation.ok() &&
              missing_create_index_generation.canonical_payload.empty(),
          "7710 admitted zero CREATE INDEX availability generation");

  auto scan_drift = fixture.bytes;
  StoreU64(&scan_drift, fixture.trailer + 768, 65535);
  const auto bad_scan =
      ipc::ValidateAndAdoptPsNarrowStatementContextResultAliasV1(
          scan_drift, scan_drift);
  Require(!bad_scan.ok() && bad_scan.canonical_payload.empty() &&
              bad_scan.outcome.diagnostic_code ==
                  "RESOURCE.BUDGET_EXCEEDED",
          "7710 admitted or misclassified an out-of-range scan bound");
}

}  // namespace

int main() {
  try {
    RequestAliasContract();
    ResultAliasContract();
  } catch (const std::exception& error) {
    (void)error;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
