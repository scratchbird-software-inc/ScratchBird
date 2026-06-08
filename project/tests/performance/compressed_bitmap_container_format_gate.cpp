// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "compressed_bitmap_container_format_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

std::vector<platform::u64> Range(platform::u64 begin, platform::u64 end) {
  std::vector<platform::u64> values;
  for (platform::u64 value = begin; value < end; ++value) {
    values.push_back(value);
  }
  return values;
}

void AppendRange(std::vector<platform::u64>* out,
                 platform::u64 begin,
                 platform::u64 end) {
  for (platform::u64 value = begin; value < end; ++value) {
    out->push_back(value);
  }
}

platform::u64 Fnv1a64(const platform::byte* data, std::size_t size) {
  platform::u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void Rechecksum(std::vector<platform::byte>* bytes) {
  Require(bytes->size() >= sizeof(platform::u64),
          "serialized payload too small for checksum");
  const auto checksum = Fnv1a64(bytes->data(), bytes->size() - sizeof(platform::u64));
  platform::StoreLittle64(bytes->data() + bytes->size() - sizeof(platform::u64),
                          checksum);
}

void RequireContainerType(
    const idx::CandidateSet& set,
    std::size_t index,
    idx::CandidateSetCompressedBitmapContainerType type,
    platform::u32 cardinality) {
  Require(index < set.compressed_bitmap_containers.size(),
          "container index missing");
  const auto& container = set.compressed_bitmap_containers[index];
  Require(container.type == type, "container type mismatch");
  Require(container.cardinality == cardinality, "container cardinality mismatch");
}

void TestContainerSelection() {
  const auto authority = Authority();

  auto sparse = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      {1, 7, 1024}, authority);
  Require(sparse.ok(), "sparse container build refused");
  Require(sparse.output.rows.empty(),
          "sparse ordinary format materialized row vector");
  Require(sparse.output.compressed_bitmap_cardinality == 3,
          "sparse cardinality drift");
  RequireContainerType(sparse.output, 0,
                       idx::CandidateSetCompressedBitmapContainerType::
                           array_sparse,
                       3);

  auto run = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      Range(100, 200), authority);
  Require(run.ok(), "run container build refused");
  RequireContainerType(run.output, 0,
                       idx::CandidateSetCompressedBitmapContainerType::run,
                       100);

  auto dense = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      Range(0, 5000), authority);
  Require(dense.ok(), "dense container build refused");
  RequireContainerType(dense.output, 0,
                       idx::CandidateSetCompressedBitmapContainerType::
                           dense_bitmap,
                       5000);

  std::vector<platform::u64> mixed = {1, 10};
  AppendRange(&mixed, 65536 + 100, 65536 + 200);
  AppendRange(&mixed, 131072, 131072 + 5000);
  auto mixed_result =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(mixed, authority,
                                                           true);
  Require(mixed_result.ok(), "mixed container build refused");
  Require(mixed_result.output.compressed_bitmap_containers.size() == 3,
          "mixed container count mismatch");
  Require(mixed_result.output.deleted_overlay_present,
          "deleted overlay presence not recorded");
  RequireContainerType(mixed_result.output, 0,
                       idx::CandidateSetCompressedBitmapContainerType::
                           array_sparse,
                       2);
  RequireContainerType(mixed_result.output, 1,
                       idx::CandidateSetCompressedBitmapContainerType::run,
                       100);
  RequireContainerType(mixed_result.output, 2,
                       idx::CandidateSetCompressedBitmapContainerType::
                           dense_bitmap,
                       5000);

  const auto inspection = idx::InspectCompressedBitmapCandidateSet(mixed_result.output);
  Require(HasEvidence(inspection, "compressed_bitmap.inspection=container_level"),
          "container-level inspection evidence missing");
  Require(HasEvidence(inspection,
                      "compressed_bitmap.materialized_row_expansion=false"),
          "inspection materialization refusal evidence missing");
  Require(HasEvidence(inspection, "candidate_set_finality_authority=false"),
          "candidate-set non-finality evidence missing");
  Require(HasEvidence(inspection, "parser_or_donor_authority=false"),
          "parser/donor non-authority evidence missing");
}

void TestSerializationRoundTripAndCorruption() {
  const auto authority = Authority();
  std::vector<platform::u64> ordinals = {1, 10};
  AppendRange(&ordinals, 65536 + 100, 65536 + 200);
  AppendRange(&ordinals, 131072, 131072 + 5000);
  auto built = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      ordinals, authority, true);
  Require(built.ok(), "round-trip source build refused");
  const auto serialized = idx::SerializeCompressedBitmapCandidateSet(built.output);
  Require(serialized.ok(), "serialization refused");
  Require(HasEvidence(serialized.evidence,
                      "compressed_bitmap.serialization=deterministic"),
          "deterministic serialization evidence missing");

  auto parsed =
      idx::DeserializeCompressedBitmapCandidateSet(serialized.serialized,
                                                  authority);
  Require(parsed.ok(), "deserialization refused");
  Require(parsed.output.compressed_bitmap_cardinality ==
              built.output.compressed_bitmap_cardinality,
          "round-trip cardinality drift");
  Require(parsed.output.compressed_bitmap_min_row_ordinal == 1,
          "round-trip min ordinal drift");
  Require(parsed.output.compressed_bitmap_max_row_ordinal == 136071,
          "round-trip max ordinal drift");
  Require(parsed.output.rows.empty(),
          "deserialization materialized row vector");
  Require(parsed.output.deleted_overlay_present,
          "round-trip deleted overlay drift");
  RequireContainerType(parsed.output, 0,
                       idx::CandidateSetCompressedBitmapContainerType::
                           array_sparse,
                       2);
  RequireContainerType(parsed.output, 1,
                       idx::CandidateSetCompressedBitmapContainerType::run,
                       100);
  RequireContainerType(parsed.output, 2,
                       idx::CandidateSetCompressedBitmapContainerType::
                           dense_bitmap,
                       5000);

  auto truncated = serialized.serialized;
  truncated.pop_back();
  auto refused = idx::DeserializeCompressedBitmapCandidateSet(truncated, authority);
  Require(!refused.ok() && refused.fail_closed,
          "truncated payload did not fail closed");

  auto bad_magic = serialized.serialized;
  bad_magic[0] = 'X';
  Rechecksum(&bad_magic);
  refused = idx::DeserializeCompressedBitmapCandidateSet(bad_magic, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() == "compressed_bitmap_bad_magic",
          "bad magic diagnostic mismatch");

  auto bad_version = serialized.serialized;
  platform::StoreLittle16(bad_version.data() + 8, 99);
  Rechecksum(&bad_version);
  refused = idx::DeserializeCompressedBitmapCandidateSet(bad_version, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "compressed_bitmap_incompatible_version",
          "bad version diagnostic mismatch");

  auto missing_contract = serialized.serialized;
  platform::StoreLittle16(missing_contract.data() + 10, 0);
  Rechecksum(&missing_contract);
  refused =
      idx::DeserializeCompressedBitmapCandidateSet(missing_contract, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_CANDIDATE_SET.RECHECK_CONTRACT_REQUIRED",
          "missing recheck contract diagnostic mismatch");
}

void TestFailClosedInputsAndAuthority() {
  const auto authority = Authority();
  auto refused = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      {2, 1}, authority);
  Require(!refused.ok() && refused.fail_closed,
          "unsorted row ordinals accepted");

  refused = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      {1, 1}, authority);
  Require(!refused.ok() && refused.fail_closed,
          "duplicate row ordinals accepted");

  auto unsafe = authority;
  unsafe.parser_or_donor_finality_or_visibility_authority = true;
  refused =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals({1, 2}, unsafe);
  Require(!refused.ok() && refused.fail_closed,
          "parser/donor authority drift accepted");

  unsafe = authority;
  unsafe.row_mga_recheck_required = false;
  refused =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals({1, 2}, unsafe);
  Require(!refused.ok() && refused.fail_closed,
          "missing MGA recheck contract accepted");

  auto built =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals({1, 2}, authority);
  Require(built.ok(), "source build for authority serialization failed");
  built.output.candidate_set_finality_authority = true;
  const auto serialized = idx::SerializeCompressedBitmapCandidateSet(built.output);
  Require(!serialized.ok() && serialized.fail_closed,
          "candidate-set finality authority serialized");

  built =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals({1, 2}, authority);
  Require(built.ok(), "source build for corrupt cardinality failed");
  ++built.output.compressed_bitmap_containers.front().cardinality;
  auto corrupt_serialized = idx::SerializeCompressedBitmapCandidateSet(built.output);
  Require(!corrupt_serialized.ok() && corrupt_serialized.fail_closed,
          "corrupt container cardinality serialized");

  built =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals({1, 2}, authority);
  Require(built.ok(), "source build for corrupt range failed");
  built.output.compressed_bitmap_min_row_ordinal = 0;
  corrupt_serialized = idx::SerializeCompressedBitmapCandidateSet(built.output);
  Require(!corrupt_serialized.ok() && corrupt_serialized.fail_closed,
          "corrupt compressed bitmap min ordinal serialized");
}

}  // namespace

int main() {
  TestContainerSelection();
  TestSerializationRoundTripAndCorruption();
  TestFailClosedInputsAndAuthority();
  std::cout << "compressed_bitmap_container_format_gate=passed\n";
  return EXIT_SUCCESS;
}
