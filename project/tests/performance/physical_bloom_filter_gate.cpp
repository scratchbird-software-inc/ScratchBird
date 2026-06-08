// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"
#include "physical_bloom_filter.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "physical_bloom_filter_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TypeUuid(platform::byte seed) {
  platform::TypedUuid typed;
  typed.kind = platform::UuidKind::object;
  for (std::size_t i = 0; i < typed.value.bytes.size(); ++i) {
    typed.value.bytes[i] = static_cast<platform::byte>(seed + i + 1);
  }
  typed.value.bytes[6] =
      static_cast<platform::byte>((typed.value.bytes[6] & 0x0f) | 0x70);
  typed.value.bytes[8] =
      static_cast<platform::byte>((typed.value.bytes[8] & 0x3f) | 0x80);
  return typed;
}

std::vector<platform::byte> SignedPayload(std::int64_t value) {
  const auto sortable = static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<platform::byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<platform::byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::string EncodedScalar(std::vector<platform::byte> payload,
                          platform::byte type_seed = 0x40) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = TypeUuid(type_seed);
  component.sort_direction = idx::IndexKeySortDirection::ascending;
  component.null_placement = idx::IndexKeyNullPlacement::nulls_last;
  component.payload = std::move(payload);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "order-preserving key encode failed");
  return std::string(reinterpret_cast<const char*>(encoded.encoded.data()),
                     encoded.encoded.size());
}

std::string IntKey(std::int64_t value) {
  return EncodedScalar(SignedPayload(value), 0x40);
}

idx::PhysicalBloomEncodedKeyEvidence Key(std::int64_t value,
                                         std::string row_uuid,
                                         std::string version_uuid) {
  idx::PhysicalBloomEncodedKeyEvidence key;
  key.row_uuid = std::move(row_uuid);
  key.version_uuid = std::move(version_uuid);
  key.encoded_key = IntKey(value);
  return key;
}

idx::PhysicalBloomAbsentProbeEvidence Absent(std::int64_t value) {
  idx::PhysicalBloomAbsentProbeEvidence probe;
  probe.encoded_key = IntKey(value);
  return probe;
}

std::vector<idx::PhysicalBloomEncodedKeyEvidence> BaseKeys() {
  return {
      Key(10,
          "11111111-1111-7111-8111-111111111101",
          "11111111-1111-7111-8111-111111111201"),
      Key(20,
          "11111111-1111-7111-8111-111111111102",
          "11111111-1111-7111-8111-111111111202"),
      Key(30,
          "11111111-1111-7111-8111-111111111103",
          "11111111-1111-7111-8111-111111111203"),
      Key(40,
          "11111111-1111-7111-8111-111111111104",
          "11111111-1111-7111-8111-111111111204"),
  };
}

std::vector<idx::PhysicalBloomAbsentProbeEvidence> AbsentSample() {
  return {Absent(1000), Absent(1001), Absent(1002), Absent(1003),
          Absent(1004), Absent(1005), Absent(1006), Absent(1007)};
}

idx::PhysicalBloomFilterBuildRequest BuildRequest() {
  idx::PhysicalBloomFilterBuildRequest request;
  request.relation_uuid = "22222222-2222-7222-8222-222222222222";
  request.index_uuid = "33333333-3333-7333-8333-333333333333";
  request.segment_uuid = "44444444-4444-7444-8444-444444444444";
  request.base_generation = 7;
  request.filter_generation = 11;
  request.seed = 0x123456789abcdef0ull;
  request.seed_version = 1;
  request.hash_count = 4;
  request.bit_count = 4096;
  request.fpr_target = 0.05;
  request.absent_probe_sample_required_for_benchmark_clean = true;
  request.authoritative_keys = BaseKeys();
  request.absent_probe_sample = AbsentSample();
  return request;
}

idx::PhysicalBloomFilterPage BuildPage() {
  const auto built = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(BuildRequest());
  Require(built.ok(), "physical Bloom build failed");
  Require(built.benchmark_clean_admissible,
          "clean build with absent sample should be benchmark-clean admissible");
  Require(built.page.bitset.size() == 512, "packed bitset byte count drifted");
  Require(built.page.layout.packed_bitset && built.page.layout.blocked_layout,
          "blocked packed layout metadata missing");
  Require(built.page.estimated_fpr <= built.page.fpr_target,
          "estimated FPR exceeds target for base fixture");
  Require(built.page.observed_absent_probe_count == AbsentSample().size(),
          "observed absent-probe count missing");
  Require(built.page.observed_fpr <= built.page.fpr_target,
          "observed FPR exceeds target for base fixture");
  return built.page;
}

bool ContainsForbiddenRuntimeArtifact(std::string_view value) {
  return value.find("docs" "/execution-plans") != std::string_view::npos ||
         value.find("execution_plan") != std::string_view::npos ||
         value.find("public_release_evidence") != std::string_view::npos ||
         value.find("docs/reference") != std::string_view::npos;
}

platform::u64 FnvChecksum(std::vector<platform::byte> bytes) {
  if (bytes.size() >= 24) {
    platform::StoreLittle64(bytes.data() + 16, 0);
  }
  platform::u64 hash = 14695981039346656037ull;
  for (platform::byte value : bytes) {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

void Rechecksum(std::vector<platform::byte>* bytes) {
  platform::StoreLittle64(bytes->data() + 16, FnvChecksum(*bytes));
}

std::size_t SkipString(const std::vector<platform::byte>& bytes,
                       std::size_t offset) {
  Require(offset + 4 <= bytes.size(), "test string offset outside payload");
  const auto size = platform::LoadLittle32(bytes.data() + offset);
  offset += 4;
  Require(offset + size <= bytes.size(), "test string payload outside payload");
  return offset + size;
}

std::size_t OffsetAfterIdentity(const std::vector<platform::byte>& bytes) {
  std::size_t offset = 24;
  offset = SkipString(bytes, offset);
  offset = SkipString(bytes, offset);
  offset = SkipString(bytes, offset);
  return offset;
}

std::size_t HashCountOffset(const std::vector<platform::byte>& bytes) {
  return OffsetAfterIdentity(bytes) + 8 + 8 + 8 + 4;
}

std::size_t BitCountOffset(const std::vector<platform::byte>& bytes) {
  return HashCountOffset(bytes) + 4;
}

idx::PhysicalBloomFilterPage VerifySerializationReopen(
    const idx::PhysicalBloomFilterPage& page) {
  const auto serialized = idx::SerializePhysicalBloomFilterPage(page);
  Require(serialized.ok(), "Bloom serialization failed");
  const auto serialized_again = idx::SerializePhysicalBloomFilterPage(page);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "Bloom serialization is not deterministic");

  idx::PhysicalBloomFilterOpenRequest open_request;
  open_request.bytes = serialized.bytes;
  open_request.expected_relation_uuid_present = true;
  open_request.expected_relation_uuid = page.relation_uuid;
  open_request.expected_index_uuid_present = true;
  open_request.expected_index_uuid = page.index_uuid;
  open_request.expected_segment_uuid_present = true;
  open_request.expected_segment_uuid = page.segment_uuid;
  open_request.expected_base_generation_present = true;
  open_request.expected_base_generation = page.base_generation;
  const auto opened = idx::OpenPhysicalBloomFilterPage(open_request);
  Require(opened.ok(), "clean Bloom reopen failed");
  const auto reserialized = idx::SerializePhysicalBloomFilterPage(opened.page);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "Bloom reopen serialization equivalence failed");
  return opened.page;
}

void VerifyProbeSemantics(const idx::PhysicalBloomFilterPage& page) {
  const auto present = idx::ProbePhysicalBloomFilter({page, IntKey(20)});
  Require(present.ok(), "present-key probe failed");
  Require(present.decision == idx::PhysicalBloomProbeDecision::maybe_present,
          "inserted key did not probe maybe-present");
  Require(!present.can_prune && present.scan_required &&
              present.exact_recheck_required &&
              present.mga_recheck_required &&
              present.security_recheck_required &&
              present.false_positive_possible,
          "maybe-present probe did not preserve exact/MGA/security recheck");

  bool found_absent = false;
  for (std::int64_t value = 1000; value < 2000; ++value) {
    const auto absent = idx::ProbePhysicalBloomFilter({page, IntKey(value)});
    Require(absent.ok(), "absent-key probe failed");
    if (absent.decision == idx::PhysicalBloomProbeDecision::definitely_absent) {
      Require(absent.can_prune && !absent.scan_required &&
                  !absent.exact_recheck_required &&
                  !absent.false_positive_possible,
              "definite absence was not a negative-prune-only decision");
      found_absent = true;
      break;
    }
  }
  Require(found_absent, "fixture could not find a definitely-absent probe");
}

void VerifyFprRefusals(const idx::PhysicalBloomFilterPage& page) {
  idx::PhysicalBloomFprMeasurementRequest missing_sample;
  missing_sample.page = page;
  missing_sample.sample_required_for_benchmark_clean = true;
  const auto missing = idx::MeasurePhysicalBloomFilterFpr(missing_sample);
  Require(!missing.ok() && !missing.benchmark_clean_admissible,
          "benchmark-clean FPR evidence was accepted without sample proof");

  auto request = BuildRequest();
  request.bit_count = idx::kPhysicalBloomFilterMinBitCount;
  request.hash_count = 4;
  request.fpr_target = 0.001;
  request.authoritative_keys.clear();
  for (int value = 0; value < 80; ++value) {
    request.authoritative_keys.push_back(Key(
        value,
        "55555555-5555-7555-8555-555555555555",
        "66666666-6666-7666-8666-666666666666"));
  }
  request.absent_probe_sample.clear();
  for (int value = 1000; value < 1040; ++value) {
    request.absent_probe_sample.push_back(Absent(value));
  }
  const auto saturated = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(saturated.ok(), "saturated Bloom fixture should still build");
  const auto measured = idx::MeasurePhysicalBloomFilterFpr(
      {saturated.page, request.absent_probe_sample, true});
  Require(measured.ok() && !measured.benchmark_clean_admissible,
          "FPR target excess did not refuse benchmark-clean evidence");
}

void VerifyMutationAndRepair(const idx::PhysicalBloomFilterPage& page) {
  idx::PhysicalBloomFilterMutation append_without_sample;
  append_without_sample.kind = idx::PhysicalBloomMutationKind::append_key;
  append_without_sample.after_key_present = true;
  append_without_sample.after_key = Key(45,
                                        "11111111-1111-7111-8111-111111111145",
                                        "11111111-1111-7111-8111-111111111245");
  const auto appended_without_sample =
      idx::ApplyPhysicalBloomFilterMutation(page, append_without_sample);
  Require(appended_without_sample.ok() &&
              appended_without_sample.page.observed_absent_probe_count == 0 &&
              appended_without_sample.page.observed_false_positive_count == 0,
          "append without absent sample retained stale observed FPR evidence");

  idx::PhysicalBloomFilterMutation append;
  append.kind = idx::PhysicalBloomMutationKind::append_key;
  append.after_key_present = true;
  append.after_key = Key(50,
                         "11111111-1111-7111-8111-111111111150",
                         "11111111-1111-7111-8111-111111111250");
  append.absent_probe_sample = AbsentSample();
  const auto appended = idx::ApplyPhysicalBloomFilterMutation(page, append);
  Require(appended.ok() && !appended.rebuild_performed,
          "append did not add bits in place");
  const auto appended_probe =
      idx::ProbePhysicalBloomFilter({appended.page, IntKey(50)});
  Require(appended_probe.decision == idx::PhysicalBloomProbeDecision::maybe_present,
          "appended key did not probe maybe-present");

  idx::PhysicalBloomFilterMutation deletion;
  deletion.kind = idx::PhysicalBloomMutationKind::delete_key;
  deletion.before_key_present = true;
  deletion.before_key = BaseKeys().front();
  const auto refused = idx::ApplyPhysicalBloomFilterMutation(appended.page, deletion);
  Require(!refused.applied && refused.filter_invalidated && refused.scan_required,
          "delete without rebuild evidence did not fail closed");

  auto repaired_keys = BaseKeys();
  repaired_keys.erase(repaired_keys.begin());
  repaired_keys.push_back(append.after_key);
  deletion.rebuild_admitted = true;
  deletion.authoritative_source_keys = repaired_keys;
  deletion.absent_probe_sample = AbsentSample();
  const auto repaired = idx::ApplyPhysicalBloomFilterMutation(appended.page, deletion);
  Require(repaired.ok() && repaired.rebuild_performed,
          "admitted delete repair did not rebuild from authoritative keys");
  const auto deleted_probe =
      idx::ProbePhysicalBloomFilter({repaired.page, BaseKeys().front().encoded_key});
  Require(deleted_probe.decision ==
              idx::PhysicalBloomProbeDecision::definitely_absent,
          "admitted rebuild retained deleted key bits in fixture");

  idx::PhysicalBloomFilterMutation empty_rebuild;
  empty_rebuild.kind = idx::PhysicalBloomMutationKind::delete_key;
  empty_rebuild.before_key_present = true;
  empty_rebuild.before_key = repaired_keys.front();
  empty_rebuild.rebuild_admitted = true;
  const auto empty = idx::ApplyPhysicalBloomFilterMutation(repaired.page,
                                                          empty_rebuild);
  Require(empty.ok() && empty.rebuild_performed &&
              empty.page.inserted_key_count == 0,
          "admitted rebuild to an empty Bloom segment was refused");
  const auto empty_probe = idx::ProbePhysicalBloomFilter({empty.page, IntKey(50)});
  Require(empty_probe.decision ==
              idx::PhysicalBloomProbeDecision::definitely_absent,
          "empty Bloom segment did not negative-prune");
}

void VerifyCorruptStaleRepair(const idx::PhysicalBloomFilterPage& page) {
  const auto serialized = idx::SerializePhysicalBloomFilterPage(page);
  Require(serialized.ok(), "corruption fixture serialization failed");

  auto bad_checksum = serialized.bytes;
  bad_checksum.back() ^= 0x01;
  const auto bad = idx::OpenPhysicalBloomFilterPage({bad_checksum});
  Require(bad.open_class == idx::PhysicalBloomFilterOpenClass::bad_checksum,
          "bad checksum was not classified exactly");

  auto stale_format = serialized.bytes;
  platform::StoreLittle32(stale_format.data() + 8, 0);
  const auto stale = idx::OpenPhysicalBloomFilterPage({stale_format});
  Require(stale.open_class == idx::PhysicalBloomFilterOpenClass::stale_format,
          "stale format was not classified exactly");

  auto invalid_hash = serialized.bytes;
  platform::StoreLittle32(invalid_hash.data() + HashCountOffset(invalid_hash), 0);
  Rechecksum(&invalid_hash);
  const auto invalid_hash_open = idx::OpenPhysicalBloomFilterPage({invalid_hash});
  Require(invalid_hash_open.open_class ==
              idx::PhysicalBloomFilterOpenClass::invalid_hash_count,
          "invalid hash count was not classified exactly");

  auto malformed_bitset = serialized.bytes;
  platform::StoreLittle64(malformed_bitset.data() + BitCountOffset(malformed_bitset),
                          128);
  Rechecksum(&malformed_bitset);
  const auto malformed_open = idx::OpenPhysicalBloomFilterPage({malformed_bitset});
  Require(malformed_open.open_class ==
              idx::PhysicalBloomFilterOpenClass::malformed_bitset_length,
          "malformed bitset length was not classified exactly");

  std::vector<platform::byte> truncated(serialized.bytes.begin(),
                                        serialized.bytes.end() - 9);
  Rechecksum(&truncated);
  const auto truncated_open = idx::OpenPhysicalBloomFilterPage({truncated});
  Require(truncated_open.open_class ==
              idx::PhysicalBloomFilterOpenClass::corrupt_payload,
          "truncated payload was not classified exactly");

  idx::PhysicalBloomFilterOpenRequest wrong_identity;
  wrong_identity.bytes = serialized.bytes;
  wrong_identity.expected_segment_uuid_present = true;
  wrong_identity.expected_segment_uuid = "77777777-7777-7777-8777-777777777777";
  const auto identity = idx::OpenPhysicalBloomFilterPage(wrong_identity);
  Require(identity.open_class == idx::PhysicalBloomFilterOpenClass::identity_mismatch,
          "identity mismatch was not classified exactly");

  idx::PhysicalBloomFilterOpenRequest stale_generation;
  stale_generation.bytes = serialized.bytes;
  stale_generation.expected_base_generation_present = true;
  stale_generation.expected_base_generation = 99;
  const auto generation = idx::OpenPhysicalBloomFilterPage(stale_generation);
  Require(generation.open_class == idx::PhysicalBloomFilterOpenClass::stale_generation,
          "stale generation was not classified exactly");

  const auto repair_refused =
      idx::RepairPhysicalBloomFilterFromEncodedKeyEvidence(page, BaseKeys(), false);
  Require(!repair_refused.applied && repair_refused.scan_required,
          "repair without explicit admission did not fail closed");
  const auto repair_admitted = idx::RepairPhysicalBloomFilterFromEncodedKeyEvidence(
      page, BaseKeys(), true, AbsentSample());
  Require(repair_admitted.ok() && repair_admitted.rebuild_performed,
          "admitted repair from authoritative keys failed");
}

void VerifyUnsafeEvidenceRefusal() {
  auto request = BuildRequest();
  request.authoritative_keys[0].encoded_key = "SBK1legacy";
  const auto unsafe = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!unsafe.ok(), "unsafe legacy key encoding was accepted");

  request = BuildRequest();
  request.authoritative_keys[0].row_uuid = "not-a-uuid";
  const auto bad_uuid = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!bad_uuid.ok(), "invalid key UUID was accepted");

  request = BuildRequest();
  request.seed = 0;
  const auto missing_seed = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!missing_seed.ok(), "missing seed was accepted");

  request = BuildRequest();
  request.hash_count = idx::kPhysicalBloomFilterMaxHashCount + 1;
  const auto bad_hash = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!bad_hash.ok(), "out-of-bounds hash count was accepted");

  request = BuildRequest();
  request.bit_count = 8;
  const auto bad_bits = idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!bad_bits.ok(), "out-of-bounds bit count was accepted");

  request = BuildRequest();
  request.authoritative_keys[0].provider_finality_authority_claimed = true;
  const auto provider_authority =
      idx::BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  Require(!provider_authority.ok(),
          "provider finality authority claim was accepted");
}

void VerifyNoExecution_PlanRuntimeArtifacts(const idx::PhysicalBloomFilterPage& page) {
  for (const auto& evidence : page.evidence) {
    Require(!ContainsForbiddenRuntimeArtifact(evidence),
            "runtime evidence contains execution_plan/doc artifact token");
  }
  const auto serialized = idx::SerializePhysicalBloomFilterPage(page);
  Require(serialized.ok(), "no-execution_plan serialization fixture failed");
  const std::string payload(reinterpret_cast<const char*>(serialized.bytes.data()),
                            serialized.bytes.size());
  Require(!ContainsForbiddenRuntimeArtifact(payload),
          "serialized runtime payload contains execution_plan/doc artifact token");
}

}  // namespace

int main() {
  const auto page = BuildPage();
  const auto reopened = VerifySerializationReopen(page);
  VerifyProbeSemantics(reopened);
  VerifyFprRefusals(reopened);
  VerifyMutationAndRepair(reopened);
  VerifyCorruptStaleRepair(reopened);
  VerifyUnsafeEvidenceRefusal();
  VerifyNoExecution_PlanRuntimeArtifacts(reopened);
  return EXIT_SUCCESS;
}
