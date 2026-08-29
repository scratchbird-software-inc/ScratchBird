// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_transaction_rollback_runtime.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace s = scratchbird::engine::sblr;

namespace {

using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint16_t ReadU16(const Bytes& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t ReadU32(const Bytes& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8 * index);
  }
  return value;
}

std::uint64_t ReadU64(const Bytes& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * index);
  }
  return value;
}

template <typename Array>
bool ArrayAt(const Bytes& bytes, std::size_t offset, const Array& expected) {
  return bytes.size() >= offset + expected.size() &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

bool TextAt(const Bytes& bytes, std::size_t offset, std::string_view expected) {
  return bytes.size() >= offset + expected.size() &&
         std::equal(expected.begin(), expected.end(), bytes.begin() + offset);
}

bool ZeroAt(const Bytes& bytes, std::size_t offset, std::size_t count) {
  return bytes.size() >= offset + count &&
         std::all_of(bytes.begin() + offset, bytes.begin() + offset + count,
                     [](std::uint8_t value) { return value == 0; });
}

template <typename Array>
bool NonZero(const Array& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

s::SblrTxnRollbackShaV1 ComputeCarrierHash(std::string_view domain,
                                           const Bytes& bytes,
                                           std::size_t hashed_bytes) {
  Bytes material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(),
                  bytes.begin() + hashed_bytes);
  return scratchbird::core::hash::ComputeSha256Digest(material).digest;
}

s::SblrTxnRollbackUuidV1 FixtureUuid(std::uint8_t discriminator) {
  s::SblrTxnRollbackUuidV1 uuid{};
  uuid[0] = discriminator;
  uuid[15] = static_cast<std::uint8_t>(discriminator ^ 0x5aU);
  return uuid;
}

s::SblrTransactionRollbackOptionsV1 MakeOptions() {
  s::SblrTransactionRollbackOptionsV1 options;
  options.transaction_uuid = FixtureUuid(0x11);
  options.local_transaction_id = 0x0102030405060708ULL;
  options.admitted_handle_evidence_sha256.fill(0x2a);
  options.rollback_mode = 1;
  options.authority_scope = 2;
  options.wait_policy = 2;
  options.deadline_monotonic_ns = 42;
  return options;
}

s::SblrTransactionRollbackResultV1 MakeResult(
    const s::SblrTransactionRollbackOptionsV1& options) {
  s::SblrTransactionRollbackResultV1 result;
  result.transaction_uuid = options.transaction_uuid;
  result.local_transaction_id = options.local_transaction_id;
  result.rollback_sequence = 0x1112131415161718ULL;
  result.rollback_policy_snapshot_uuid = FixtureUuid(0x22);
  result.rollback_policy_generation = 7;
  result.lifecycle_state = 3;
  result.authority_scope = options.authority_scope;
  result.executor_availability_generation = 9;
  return result;
}

bool DecodeOptions(const Bytes& bytes) {
  s::SblrTransactionRollbackOptionsV1 decoded;
  std::string detail;
  return s::DecodeSblrTransactionRollbackOptionsV1(
      bytes.data(), bytes.size(), &decoded, &detail);
}

bool DecodeResult(const Bytes& bytes) {
  s::SblrTransactionRollbackResultV1 decoded;
  std::string detail;
  return s::DecodeSblrTransactionRollbackResultV1(
      bytes.data(), bytes.size(), &decoded, &detail);
}

struct ByteMutation {
  const char* failure_message;
  std::size_t offset;
};

template <typename Decoder>
void ExpectByteMutationsRejected(const Bytes& canonical,
                                 const std::vector<ByteMutation>& mutations,
                                 Decoder decode) {
  for (const auto& mutation : mutations) {
    auto malformed = canonical;
    malformed[mutation.offset] ^= 1;
    Require(!decode(malformed), mutation.failure_message);
  }
}

template <typename Carrier>
struct EncoderMutation {
  const char* failure_message;
  void (*apply)(Carrier*);
};

template <typename Carrier, typename Encoder>
void ExpectEncoderMutationsRejected(
    const Carrier& canonical,
    const std::vector<EncoderMutation<Carrier>>& mutations, Encoder encode) {
  for (const auto& mutation : mutations) {
    auto malformed = canonical;
    mutation.apply(&malformed);
    Require(encode(malformed).empty(), mutation.failure_message);
  }
}

void VerifyOptionsLayout(
    const s::SblrTransactionRollbackOptionsV1& options,
    const Bytes& bytes) {
  Require(bytes.size() == 120, "TXRO extent drifted");
  Require(TextAt(bytes, 0, "TXRO"), "TXRO magic offset drifted");
  Require(ReadU16(bytes, 4) == 1, "TXRO version offset drifted");
  Require(ReadU16(bytes, 6) == 120, "TXRO header extent drifted");
  Require(ReadU32(bytes, 8) == 120, "TXRO total extent drifted");
  Require(ReadU32(bytes, 12) == 0, "TXRO flags drifted");
  Require(ArrayAt(bytes, 16, options.transaction_uuid),
          "TXRO transaction UUID offset drifted");
  Require(ReadU64(bytes, 32) == options.local_transaction_id,
          "TXRO local transaction ID offset drifted");
  Require(ArrayAt(bytes, 40, options.admitted_handle_evidence_sha256),
          "TXRO admitted handle evidence offset drifted");
  Require(bytes[72] == options.rollback_mode,
          "TXRO rollback mode offset drifted");
  Require(bytes[73] == options.authority_scope,
          "TXRO authority scope offset drifted");
  Require(bytes[74] == options.wait_policy,
          "TXRO wait policy offset drifted");
  Require(ZeroAt(bytes, 75, 5), "TXRO reserved bytes drifted");
  Require(ReadU64(bytes, 80) == options.deadline_monotonic_ns,
          "TXRO deadline offset drifted");
  const auto expected_hash = ComputeCarrierHash(
      "ScratchBird.SblrTransactionRollbackOptions.V1", bytes, 88);
  Require(NonZero(expected_hash), "TXRO computed options hash is zero");
  Require(options.options_sha256 == expected_hash,
          "TXRO options hash domain drifted");
  Require(ArrayAt(bytes, 88, expected_hash),
          "TXRO options hash offset drifted");
}

void VerifyResultLayout(const s::SblrTransactionRollbackResultV1& result,
                        const Bytes& bytes) {
  Require(bytes.size() == 120, "TXRR extent drifted");
  Require(TextAt(bytes, 0, "TXRR"), "TXRR magic offset drifted");
  Require(ReadU16(bytes, 4) == 1, "TXRR version offset drifted");
  Require(ReadU16(bytes, 6) == 120, "TXRR header extent drifted");
  Require(ReadU32(bytes, 8) == 120, "TXRR total extent drifted");
  Require(ReadU32(bytes, 12) == 0, "TXRR flags drifted");
  Require(ArrayAt(bytes, 16, result.transaction_uuid),
          "TXRR transaction UUID offset drifted");
  Require(ReadU64(bytes, 32) == result.local_transaction_id,
          "TXRR local transaction ID offset drifted");
  Require(ReadU64(bytes, 40) == result.rollback_sequence,
          "TXRR rollback sequence offset drifted");
  Require(ArrayAt(bytes, 48, result.rollback_policy_snapshot_uuid),
          "TXRR rollback policy UUID offset drifted");
  Require(ReadU64(bytes, 64) == result.rollback_policy_generation,
          "TXRR rollback policy generation offset drifted");
  Require(bytes[72] == result.lifecycle_state,
          "TXRR lifecycle state offset drifted");
  Require(bytes[73] == result.authority_scope,
          "TXRR authority scope offset drifted");
  Require(ZeroAt(bytes, 74, 6), "TXRR reserved bytes drifted");
  const auto expected_evidence = ComputeCarrierHash(
      "ScratchBird.SblrTransactionRollbackResult.V1", bytes, 80);
  Require(NonZero(expected_evidence), "TXRR computed result evidence is zero");
  Require(ArrayAt(bytes, 80, expected_evidence),
          "TXRR evidence hash domain or offset drifted");
  Require(ReadU64(bytes, 112) == result.executor_availability_generation,
          "TXRR executor availability offset drifted");
}

}  // namespace

int main() {
  // CSC-TEST-002354: exact TXRO120/TXRR120 structural codec coverage.
  auto options = MakeOptions();
  Require(!NonZero(options.options_sha256),
          "TXRO fixture supplied an options hash");
  const auto options_bytes =
      s::EncodeSblrTransactionRollbackOptionsV1(&options);
  VerifyOptionsLayout(options, options_bytes);

  s::SblrTransactionRollbackOptionsV1 decoded_options;
  std::string detail;
  Require(s::DecodeSblrTransactionRollbackOptionsV1(
              options_bytes.data(), options_bytes.size(), &decoded_options,
              &detail),
          "canonical TXRO refused");
  Require(decoded_options.transaction_uuid == options.transaction_uuid &&
              decoded_options.local_transaction_id ==
                  options.local_transaction_id &&
              decoded_options.admitted_handle_evidence_sha256 ==
                  options.admitted_handle_evidence_sha256 &&
              decoded_options.rollback_mode == options.rollback_mode &&
              decoded_options.authority_scope == options.authority_scope &&
              decoded_options.wait_policy == options.wait_policy &&
              decoded_options.deadline_monotonic_ns ==
                  options.deadline_monotonic_ns &&
              decoded_options.options_sha256 == options.options_sha256,
          "TXRO round trip changed a field");
  auto reencoded_options = decoded_options;
  Require(s::EncodeSblrTransactionRollbackOptionsV1(&reencoded_options) ==
              options_bytes,
          "TXRO canonical re-encode drifted");

  const std::vector<EncoderMutation<s::SblrTransactionRollbackOptionsV1>>
      invalid_options{
          {"TXRO nil transaction UUID encoded",
           [](auto* value) { value->transaction_uuid = {}; }},
          {"TXRO zero local transaction ID encoded",
           [](auto* value) { value->local_transaction_id = 0; }},
          {"TXRO zero admitted handle evidence encoded", [](auto* value) {
             value->admitted_handle_evidence_sha256.fill(0);
           }},
          {"TXRO rollback mode zero encoded",
           [](auto* value) { value->rollback_mode = 0; }},
          {"TXRO non-full rollback mode encoded",
           [](auto* value) { value->rollback_mode = 2; }},
          {"TXRO authority scope zero encoded",
           [](auto* value) { value->authority_scope = 0; }},
          {"TXRO authority scope above enum encoded",
           [](auto* value) { value->authority_scope = 3; }},
          {"TXRO wait policy zero encoded",
           [](auto* value) { value->wait_policy = 0; }},
          {"TXRO wait policy above enum encoded",
           [](auto* value) { value->wait_policy = 3; }},
      };
  ExpectEncoderMutationsRejected(
      MakeOptions(), invalid_options,
      [](auto value) {
        return s::EncodeSblrTransactionRollbackOptionsV1(&value);
      });

  ExpectByteMutationsRejected(
      options_bytes,
      {{"TXRO wrong magic decoded", 0},
       {"TXRO wrong version decoded", 4},
       {"TXRO wrong header extent decoded", 6},
       {"TXRO wrong total extent decoded", 8},
       {"TXRO nonzero flags decoded", 12},
       {"TXRO reserved byte decoded", 75},
       {"TXRO mutated body decoded", 16},
       {"TXRO mutated options hash decoded", 88}},
      DecodeOptions);
  auto truncated_options = options_bytes;
  truncated_options.pop_back();
  Require(!DecodeOptions(truncated_options), "truncated TXRO decoded");
  auto trailing_options = options_bytes;
  trailing_options.push_back(0);
  Require(!DecodeOptions(trailing_options), "TXRO trailing byte decoded");
  Require(!s::DecodeSblrTransactionRollbackOptionsV1(
              nullptr, options_bytes.size(), &decoded_options, &detail),
          "null TXRO bytes decoded");
  Require(!s::DecodeSblrTransactionRollbackOptionsV1(
              options_bytes.data(), options_bytes.size(), nullptr, &detail),
          "TXRO decoded through null output");

  const auto result = MakeResult(options);
  Require(!NonZero(result.rollback_evidence_sha256),
          "TXRR fixture supplied result evidence");
  const auto result_bytes = s::EncodeSblrTransactionRollbackResultV1(result);
  Require(!NonZero(result.rollback_evidence_sha256),
          "TXRR encoder rewrote caller-owned result evidence");
  VerifyResultLayout(result, result_bytes);

  s::SblrTransactionRollbackResultV1 decoded_result;
  Require(s::DecodeSblrTransactionRollbackResultV1(
              result_bytes.data(), result_bytes.size(), &decoded_result,
              &detail),
          "canonical TXRR refused");
  const auto expected_result_evidence = ComputeCarrierHash(
      "ScratchBird.SblrTransactionRollbackResult.V1", result_bytes, 80);
  Require(decoded_result.transaction_uuid == result.transaction_uuid &&
              decoded_result.local_transaction_id ==
                  result.local_transaction_id &&
              decoded_result.rollback_sequence == result.rollback_sequence &&
              decoded_result.rollback_policy_snapshot_uuid ==
                  result.rollback_policy_snapshot_uuid &&
              decoded_result.rollback_policy_generation ==
                  result.rollback_policy_generation &&
              decoded_result.lifecycle_state == result.lifecycle_state &&
              decoded_result.authority_scope == result.authority_scope &&
              decoded_result.rollback_evidence_sha256 ==
                  expected_result_evidence &&
              decoded_result.executor_availability_generation ==
                  result.executor_availability_generation,
          "TXRR round trip changed a field");
  Require(s::EncodeSblrTransactionRollbackResultV1(decoded_result) ==
              result_bytes,
          "TXRR canonical re-encode drifted");

  const std::vector<EncoderMutation<s::SblrTransactionRollbackResultV1>>
      invalid_results{
          {"TXRR nil transaction UUID encoded",
           [](auto* value) { value->transaction_uuid = {}; }},
          {"TXRR zero local transaction ID encoded",
           [](auto* value) { value->local_transaction_id = 0; }},
          {"TXRR zero rollback sequence encoded",
           [](auto* value) { value->rollback_sequence = 0; }},
          {"TXRR nil rollback policy UUID encoded",
           [](auto* value) { value->rollback_policy_snapshot_uuid = {}; }},
          {"TXRR zero rollback policy generation encoded",
           [](auto* value) { value->rollback_policy_generation = 0; }},
          {"TXRR non-rolled-back lifecycle encoded",
           [](auto* value) { value->lifecycle_state = 1; }},
          {"TXRR authority scope zero encoded",
           [](auto* value) { value->authority_scope = 0; }},
          {"TXRR authority scope above enum encoded",
           [](auto* value) { value->authority_scope = 3; }},
          {"TXRR zero executor availability encoded", [](auto* value) {
             value->executor_availability_generation = 0;
           }},
          {"TXRR caller-supplied wrong result evidence encoded",
           [](auto* value) { value->rollback_evidence_sha256.fill(0xff); }},
      };
  ExpectEncoderMutationsRejected(
      result, invalid_results,
      [](const auto& value) {
        return s::EncodeSblrTransactionRollbackResultV1(value);
      });

  ExpectByteMutationsRejected(
      result_bytes,
      {{"TXRR wrong magic decoded", 0},
       {"TXRR wrong version decoded", 4},
       {"TXRR wrong header extent decoded", 6},
       {"TXRR wrong total extent decoded", 8},
       {"TXRR nonzero flags decoded", 12},
       {"TXRR reserved byte decoded", 74},
       {"TXRR mutated body decoded", 16},
       {"TXRR mutated evidence hash decoded", 80}},
      DecodeResult);
  auto truncated_result = result_bytes;
  truncated_result.pop_back();
  Require(!DecodeResult(truncated_result), "truncated TXRR decoded");
  auto trailing_result = result_bytes;
  trailing_result.push_back(0);
  Require(!DecodeResult(trailing_result), "TXRR trailing byte decoded");
  Require(!s::DecodeSblrTransactionRollbackResultV1(
              nullptr, result_bytes.size(), &decoded_result, &detail),
          "null TXRR bytes decoded");
  Require(!s::DecodeSblrTransactionRollbackResultV1(
              result_bytes.data(), result_bytes.size(), nullptr, &detail),
          "TXRR decoded through null output");

  return EXIT_SUCCESS;
}
