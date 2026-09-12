// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_savepoint_runtime.hpp"
#include "hash_digest.hpp"
#include <openssl/evp.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace s = scratchbird::engine::sblr;
namespace {
using Bytes = std::vector<std::uint8_t>;
unsigned checks = 0, failures = 0;
void Check(bool ok, std::string_view label) {
  ++checks;
  if (!ok) {
    ++failures;
    if (failures <= 30) std::cerr << label << '\n';
  }
}
void Number(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) bytes.at(offset + i) = value >> (8 * i);
}
void Seal(Bytes& bytes, std::string_view domain, std::size_t offset) {
  if (domain.empty()) return;
  Bytes material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.begin() + offset);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  Check(digest.ok(), "independent SHA computation failed");
  std::copy(digest.digest.begin(), digest.digest.end(), bytes.begin() + offset);
}

// Offsets and hash domains below are transcribed from the manifest-admitted
// sblr-operand-descriptors.yaml transaction savepoint/release/rollback rows.
// No production encoder constructs the admission oracle. These carriers hold
// system identities only; this test says nothing about UUID-typed user data.
template<class Value, class Encode, class Decode>
void Exercise(std::string_view magic, std::size_t size, Encode encode, Decode decode,
              std::initializer_list<std::size_t> identities,
              std::initializer_list<std::size_t> numbers,
              std::initializer_list<std::size_t> hashes,
              std::initializer_list<std::size_t> reserved,
              std::size_t lifecycle = 0, std::string_view domain = {},
              std::size_t digest_offset = 0) {
  Bytes wire(size, 0);
  std::copy(magic.begin(), magic.end(), wire.begin());
  Number(wire, 4, 1, 2);
  Number(wire, 6, size, 2);
  Number(wire, 8, size, 4);
  for (auto offset : identities) {
    for (unsigned i = 0; i < 16; ++i) wire.at(offset + i) = offset + i;
    wire.at(offset + 6) = (wire.at(offset + 6) & 15) | 0x70;
    wire.at(offset + 8) = (wire.at(offset + 8) & 63) | 0x80;
  }
  for (auto offset : numbers) Number(wire, offset, offset + 1, 8);
  for (auto offset : hashes)
    for (unsigned i = 0; i < 32; ++i) wire.at(offset + i) = offset + i + 1;
  if (lifecycle) wire.at(lifecycle) = 1;
  Seal(wire, domain, digest_offset);
  Value valid;
  std::string detail;
  const bool admitted = decode(wire.data(), wire.size(), &valid, &detail);
  Check(admitted, std::string(magic) + " independent canonical carrier refused");
  if (!admitted) return;
  Check(encode(valid) == wire, std::string(magic) + " canonical bytes changed");
  auto reject_identity_encoding = [&](s::SpUuid Value::* field) {
    auto invalid = valid;
    (invalid.*field).fill(0);
    Check(encode(invalid).empty(), "encoder accepted nil system identity");
    for (unsigned version = 0; version < 16; ++version) {
      if (version == 7) continue;
      invalid = valid;
      (invalid.*field)[6] = ((invalid.*field)[6] & 15) | (version << 4);
      Check(encode(invalid).empty(), "encoder accepted non-v7 system identity");
    }
    for (unsigned variant : {0U, 1U, 3U}) {
      invalid = valid;
      (invalid.*field)[8] = ((invalid.*field)[8] & 63) | (variant << 6);
      Check(encode(invalid).empty(), "encoder accepted non-RFC system identity");
    }
  };
  if constexpr (requires { valid.preliminary_receipt_uuid; })
    reject_identity_encoding(&Value::preliminary_receipt_uuid);
  if constexpr (requires { valid.descriptor_uuid; })
    reject_identity_encoding(&Value::descriptor_uuid);
  if constexpr (requires { valid.savepoint_uuid; })
    reject_identity_encoding(&Value::savepoint_uuid);
  if constexpr (requires { valid.transaction_uuid; })
    reject_identity_encoding(&Value::transaction_uuid);
  if constexpr (requires { valid.released_savepoint_uuid; })
    reject_identity_encoding(&Value::released_savepoint_uuid);
  if constexpr (requires { valid.target_savepoint_uuid; })
    reject_identity_encoding(&Value::target_savepoint_uuid);
  auto reject = [&](const Bytes& bad) {
    auto output = valid;
    Check(!decode(bad.data(), bad.size(), &output, &detail),
          std::string(magic) + " malformed carrier admitted");
    Check(encode(output) == wire, std::string(magic) + " failed read mutated output");
  };
  // Independently reseal mutations so UUID/header/field admission is tested,
  // rather than incidentally failing only because an old hash is mismatched.
  auto mutate = [&](std::size_t offset, std::uint8_t value) {
    auto bad = wire;
    bad.at(offset) = value;
    Seal(bad, domain, digest_offset);
    reject(bad);
  };
  for (std::size_t n = 0; n < size; ++n) reject(Bytes(wire.begin(), wire.begin() + n));
  auto longer = wire;
  longer.push_back(0);
  reject(longer);
  for (unsigned i = 0; i < 16; ++i) mutate(i, wire[i] ^ 0x80);
  for (auto offset : reserved) mutate(offset, 1);
  if (lifecycle) {
    mutate(lifecycle, 0);
    mutate(lifecycle, 2);
  }
  for (auto offset : numbers) {
    auto bad = wire;
    Number(bad, offset, 0, 8);
    Seal(bad, domain, digest_offset);
    reject(bad);
  }
  for (auto offset : identities) {
    auto bad = wire;
    std::fill_n(bad.begin() + offset, 16, 0);
    Seal(bad, domain, digest_offset);
    reject(bad);
    for (unsigned version = 0; version < 16; ++version)
      if (version != 7) mutate(offset + 6, (wire[offset + 6] & 15) | (version << 4));
    for (unsigned variant : {0U, 1U, 3U})
      mutate(offset + 8, (wire[offset + 8] & 63) | (variant << 6));
  }
  for (auto offset : hashes) {
    auto bad = wire;
    std::fill_n(bad.begin() + offset, 32, 0);
    // External evidence must be present. Embedded checksums must be exact,
    // including a refusal of the encoder's zero-digest generation sentinel.
    if (offset != digest_offset || domain.empty()) Seal(bad, domain, digest_offset);
    reject(bad);
  }
  if (!domain.empty()) {
    auto bad = wire;
    bad.at(digest_offset) ^= 1;
    reject(bad);
  }
  auto output = valid;
  Check(!decode(nullptr, size, &output, &detail), "null input admitted");
  Check(encode(output) == wire, "null input changed output");
  // The baseline SPRO decoder dereferences null output. Run this only after
  // all ordinary admission checks pass, retaining baseline failure evidence.
  if (!failures) Check(!decode(wire.data(), size, nullptr, &detail), "null output admitted");
  if (!domain.empty()) {
    auto generated = valid;
    if constexpr (requires { generated.savepoint_evidence_sha256; })
      generated.savepoint_evidence_sha256.fill(0);
    if constexpr (requires { generated.release_evidence_sha256; })
      generated.release_evidence_sha256.fill(0);
    if constexpr (requires { generated.rollback_evidence_sha256; })
      generated.rollback_evidence_sha256.fill(0);
    if constexpr (requires { generated.descriptor_evidence_sha256; })
      generated.descriptor_evidence_sha256.fill(0);
    Check(encode(generated) == wire, "output digest generation changed canonical bytes");
    // Actual SHA provider unavailability, confined to this test process.
    // Restore the provider before evaluating any later oracle or carrier.
    Check(EVP_set_default_properties(nullptr, "provider=sb_missing_test_provider") == 1,
          "SHA provider failure setup failed");
    const auto failed_encoding = encode(generated);
    Check(EVP_set_default_properties(nullptr, "") == 1, "SHA provider restore failed");
    Check(failed_encoding.empty(), "encoder published evidence despite actual SHA failure");
  }
}
}

int main() {
  Exercise<s::SblrSavepointDescriptorV1>("SPDO",128,
      s::EncodeSblrSavepointDescriptorV1,s::DecodeSblrSavepointDescriptorV1,
      {16,40,64},{32,56,80,88},{96},{});
  Exercise<s::SblrSavepointHandleV1>("SPHD",144,
      s::EncodeSblrSavepointHandleV1,s::DecodeSblrSavepointHandleV1,
      {16,40},{32,56,64,72,120},{88},
      {81,82,83,84,85,86,87,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143},
      80,"ScratchBird.SblrSavepointHandle.V1",88);
  Exercise<s::SblrSavepointReleaseOperandV1>("SPRL",128,
      s::EncodeSblrSavepointReleaseOperandV1,s::DecodeSblrSavepointReleaseOperandV1,
      {16,40},{32,56,64,72,112},{80},{120,121,122,123,124,125,126,127});
  Exercise<s::SblrSavepointReleaseResultV1>("SPRR",120,
      s::EncodeSblrSavepointReleaseResultV1,s::DecodeSblrSavepointReleaseResultV1,
      {16,40},{32,56,64,72,112},{80},{},0,"ScratchBird.SblrSavepointReleaseResult.V1",80);
  Exercise<s::SblrSavepointRollbackOperandV1>("SPRO",128,
      s::EncodeSblrSavepointRollbackOperandV1,s::DecodeSblrSavepointRollbackOperandV1,
      {16,40},{32,56,64,72,112},{80},{120,121,122,123,124,125,126,127});
  Exercise<s::SblrSavepointRollbackResultV1>("SPRB",168,
      s::EncodeSblrSavepointRollbackResultV1,s::DecodeSblrSavepointRollbackResultV1,
      {16,40},{32,56,64,72,80,160},{96,128},{89,90,91,92,93,94,95},88,
      "ScratchBird.SblrSavepointRollbackResult.V1",128);
  Exercise<s::SblrSavepointCoordinationRequestV1>("SPCR",128,
      s::EncodeSblrSavepointCoordinationRequestV1,s::DecodeSblrSavepointCoordinationRequestV1,
      {16,32},{48,88},{56,96},{});
  Exercise<s::SblrSavepointCoordinationResultV1>("SPCD",160,
      s::EncodeSblrSavepointCoordinationResultV1,s::DecodeSblrSavepointCoordinationResultV1,
      {16,32,56},{48,72,80,88},{96,128},{},0,"ScratchBird.SblrSavepointCoordination.V1",128);
  std::cout << "savepoint_wire_admission checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
