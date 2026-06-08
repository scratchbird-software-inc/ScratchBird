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
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "compressed_bitmap_algebra_gate: " << message << '\n';
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

std::set<platform::u64> ToSet(const std::vector<platform::u64>& values) {
  return {values.begin(), values.end()};
}

std::vector<platform::u64> UnionValues(const std::vector<platform::u64>& left,
                                       const std::vector<platform::u64>& right) {
  auto out = ToSet(left);
  out.insert(right.begin(), right.end());
  return {out.begin(), out.end()};
}

std::vector<platform::u64> IntersectValues(
    const std::vector<platform::u64>& left,
    const std::vector<platform::u64>& right) {
  const auto right_set = ToSet(right);
  std::vector<platform::u64> out;
  for (const auto value : left) {
    if (right_set.contains(value)) {
      out.push_back(value);
    }
  }
  return out;
}

std::vector<platform::u64> SubtractValues(
    const std::vector<platform::u64>& left,
    const std::vector<platform::u64>& right) {
  const auto right_set = ToSet(right);
  std::vector<platform::u64> out;
  for (const auto value : left) {
    if (!right_set.contains(value)) {
      out.push_back(value);
    }
  }
  return out;
}

bool ContainsOrdinal(const idx::CandidateSet& set, platform::u64 ordinal) {
  for (const auto& container : set.compressed_bitmap_containers) {
    if (ordinal < container.base_row_ordinal ||
        ordinal >= container.base_row_ordinal + container.ordinal_span) {
      continue;
    }
    const auto offset =
        static_cast<platform::u32>(ordinal - container.base_row_ordinal);
    switch (container.type) {
      case idx::CandidateSetCompressedBitmapContainerType::array_sparse:
        return std::binary_search(container.array_offsets.begin(),
                                  container.array_offsets.end(),
                                  static_cast<platform::u16>(offset));
      case idx::CandidateSetCompressedBitmapContainerType::run:
        for (const auto& run : container.runs) {
          if (offset >= run.start_offset &&
              offset < run.start_offset + run.run_length) {
            return true;
          }
        }
        return false;
      case idx::CandidateSetCompressedBitmapContainerType::dense_bitmap:
        return (container.bitmap_words[offset / 64] &
                (1ull << (offset % 64))) != 0;
    }
  }
  return false;
}

void RequireCompressedResult(const idx::CandidateSetResult& result,
                             platform::u64 cardinality,
                             std::string_view action) {
  Require(result.ok(), "compressed bitmap algebra refused");
  Require(result.output.encoding == idx::CandidateSetEncoding::compressed_bitmap,
          "compressed algebra did not preserve compressed encoding");
  Require(result.output.rows.empty(),
          "compressed algebra materialized row vector");
  Require(result.output.compressed_bitmap_cardinality == cardinality,
          "compressed algebra cardinality mismatch");
  Require(HasEvidence(result.evidence, "compressed_bitmap.algebra=container_level"),
          "container-level algebra evidence missing");
  Require(HasEvidence(result.evidence,
                      "compressed_bitmap.materialized_row_expansion=false"),
          "materialization evidence missing");
  Require(HasEvidence(result.evidence, "compatibility_bridge.used=false"),
          "compatibility bridge was not excluded in evidence");
  Require(HasEvidence(result.evidence, action), "operation action missing");
  Require(HasEvidence(result.evidence, "candidate_set_finality_authority=false"),
          "candidate-set non-authority evidence missing");
  Require(HasEvidence(result.evidence, "parser_or_donor_authority=false"),
          "parser/donor non-authority evidence missing");
  Require(HasEvidence(result.evidence, "provider_finality_authority=false"),
          "provider non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "wal_recovery_or_finality_authority=false"),
          "WAL non-authority evidence missing");
  Require(HasEvidence(result.evidence, "exact_recheck.required=true"),
          "exact recheck evidence missing");
  Require(HasEvidence(result.evidence, "mga_visibility_recheck.required=true"),
          "MGA recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "security_authorization_recheck.required=true"),
          "security recheck evidence missing");
}

idx::CandidateSet Build(const std::vector<platform::u64>& ordinals,
                        bool deleted_overlay_present = false) {
  auto result = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      ordinals, Authority(), deleted_overlay_present);
  Require(result.ok(), "compressed bitmap source build refused");
  return result.output;
}

idx::CandidateSet ExactSet() {
  idx::CandidateSet set;
  set.encoding = idx::CandidateSetEncoding::exact_row_uuid_ordered_stream;
  set.row_uuid_ordered = true;
  set.compressed = false;
  set.requires_exact_recheck = true;
  set.requires_mga_visibility_recheck = true;
  set.requires_security_authorization_recheck = true;
  return set;
}

void ExerciseBinaryShape(const std::vector<platform::u64>& left_values,
                         const std::vector<platform::u64>& right_values) {
  const auto authority = Authority();
  const auto left = Build(left_values);
  const auto right = Build(right_values);

  auto expected = UnionValues(left_values, right_values);
  auto result = idx::UnionCandidateSets(left, right, authority);
  RequireCompressedResult(result, expected.size(), "or_union");
  for (const auto value : expected) {
    Require(ContainsOrdinal(result.output, value), "union missing ordinal");
  }

  expected = IntersectValues(left_values, right_values);
  result = idx::IntersectCandidateSets(left, right, authority);
  RequireCompressedResult(result, expected.size(), "and_intersection");
  for (const auto value : expected) {
    Require(ContainsOrdinal(result.output, value),
            "intersection missing ordinal");
  }

  expected = SubtractValues(left_values, right_values);
  result = idx::SubtractCandidateSets(left, right, authority);
  RequireCompressedResult(result, expected.size(), "andnot_subtract");
  for (const auto value : expected) {
    Require(ContainsOrdinal(result.output, value), "subtract missing ordinal");
  }
}

void TestSparseRunDenseAndMixedAlgebra() {
  ExerciseBinaryShape({1, 7, 1024}, {7, 9, 2048});
  ExerciseBinaryShape(Range(100, 200), Range(150, 250));
  ExerciseBinaryShape(Range(0, 5000), Range(2500, 7500));

  std::vector<platform::u64> mixed_left = {1, 10};
  AppendRange(&mixed_left, 65536 + 100, 65536 + 200);
  AppendRange(&mixed_left, 131072, 131072 + 5000);
  std::vector<platform::u64> mixed_right = {10, 14};
  AppendRange(&mixed_right, 65536 + 150, 65536 + 250);
  AppendRange(&mixed_right, 133000, 133000 + 5000);
  ExerciseBinaryShape(mixed_left, mixed_right);
}

void TestComplementTopKAndPopcount() {
  const auto authority = Authority();
  const auto input = Build({2, 4});
  auto complement =
      idx::ComplementCandidateSetWithinUniverse(input, 1, 5, authority);
  RequireCompressedResult(complement, 3, "not_complement");
  Require(ContainsOrdinal(complement.output, 1), "complement missing 1");
  Require(ContainsOrdinal(complement.output, 3), "complement missing 3");
  Require(ContainsOrdinal(complement.output, 5), "complement missing 5");
  Require(!ContainsOrdinal(complement.output, 2), "complement retained 2");
  Require(HasEvidence(complement.evidence, "compressed_bitmap.universe_min=1"),
          "complement universe min evidence missing");
  Require(HasEvidence(complement.evidence, "compressed_bitmap.universe_max=5"),
          "complement universe max evidence missing");

  const auto first_k_input = Build({5, 7, 10, 65537});
  auto top_k = idx::TopKCandidateSet(first_k_input, 3, authority);
  RequireCompressedResult(top_k, 3, "first_k_ordinal");
  Require(ContainsOrdinal(top_k.output, 5), "top-k missing first ordinal");
  Require(ContainsOrdinal(top_k.output, 7), "top-k missing second ordinal");
  Require(ContainsOrdinal(top_k.output, 10), "top-k missing third ordinal");
  Require(!ContainsOrdinal(top_k.output, 65537), "top-k kept fourth ordinal");
  Require(HasEvidence(top_k.evidence, "top_k.action=first_k_ordinal"),
          "top-k first-k evidence missing");

  auto popcount = idx::CandidateSetPopcount(first_k_input, authority);
  Require(popcount.ok(), "popcount refused");
  Require(popcount.cardinality == 4, "popcount cardinality mismatch");
  Require(HasEvidence(popcount.evidence, "operation=popcount"),
          "popcount operation evidence missing");
  Require(HasEvidence(popcount.evidence,
                      "compressed_bitmap.materialized_row_expansion=false"),
          "popcount materialization evidence missing");
  Require(HasEvidence(popcount.evidence, "compatibility_bridge.used=false"),
          "popcount bridge evidence missing");
}

void TestDeletedOverlayHandling() {
  const auto authority = Authority();
  const auto overlay = Build({1, 2, 3}, true);
  const auto ordinary = Build({3, 4});
  const auto inspection = idx::InspectCompressedBitmapCandidateSet(overlay);
  Require(HasEvidence(inspection, "compressed_bitmap.deleted_overlay_present=true"),
          "deleted overlay inspection did not propagate");

  auto unioned = idx::UnionCandidateSets(overlay, ordinary, authority);
  Require(unioned.ok(), "deleted overlay union was not preserved");
  Require(unioned.output.deleted_overlay_present,
          "deleted overlay union lost overlay flag");
  Require(HasEvidence(unioned.evidence,
                      "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck"),
          "deleted overlay union did not record exact-recheck preservation");

  auto top_k = idx::TopKCandidateSet(overlay, 1, authority);
  Require(top_k.ok(), "deleted overlay top-k was not preserved");
  Require(top_k.output.deleted_overlay_present,
          "deleted overlay top-k lost overlay flag");
  Require(HasEvidence(top_k.evidence,
                      "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck"),
          "deleted overlay top-k did not record exact-recheck preservation");

  auto complement = idx::ComplementCandidateSetWithinUniverse(overlay, 1, 4, authority);
  Require(complement.ok(), "deleted overlay complement was not preserved");
  Require(complement.output.deleted_overlay_present,
          "deleted overlay complement lost overlay flag");
  Require(HasEvidence(complement.evidence,
                      "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck"),
          "deleted overlay complement did not record exact-recheck preservation");

  auto popcount = idx::CandidateSetPopcount(overlay, authority);
  Require(popcount.ok(), "deleted-overlay raw popcount refused");
  Require(HasEvidence(popcount.evidence,
                      "compressed_bitmap.deleted_overlay_present=true"),
          "deleted-overlay popcount evidence missing");
}

void TestFailClosedContractsAndCorruption() {
  const auto authority = Authority();
  auto left = Build({1, 3, 5});
  auto right = Build({3, 5, 7});

  auto corrupt = left;
  std::swap(corrupt.compressed_bitmap_containers.front().array_offsets[0],
            corrupt.compressed_bitmap_containers.front().array_offsets[1]);
  auto refused = idx::UnionCandidateSets(corrupt, right, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "invalid_compressed_bitmap_container_representation",
          "corrupt unsorted container accepted");

  corrupt = left;
  corrupt.requires_mga_visibility_recheck = false;
  refused = idx::UnionCandidateSets(corrupt, right, authority);
  Require(!refused.ok() && refused.fail_closed,
          "missing MGA recheck contract accepted");

  corrupt = left;
  corrupt.parser_or_donor_finality_or_visibility_authority = true;
  refused = idx::UnionCandidateSets(corrupt, right, authority);
  Require(!refused.ok() && refused.fail_closed,
          "parser/donor authority drift accepted");

  corrupt = left;
  corrupt.final_rows_authorized = true;
  refused = idx::UnionCandidateSets(corrupt, right, authority);
  Require(!refused.ok() && refused.fail_closed,
          "compressed algebra accepted final authorized rows");

  refused = idx::ComplementCandidateSetWithinUniverse(left, 2, 3, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "compressed_bitmap_universe_does_not_cover_input",
          "unsafe complement universe accepted");

  auto unsafe_authority = authority;
  unsafe_authority.provider_finality_or_visibility_authority = true;
  refused = idx::UnionCandidateSets(left, right, unsafe_authority);
  Require(!refused.ok() && refused.fail_closed,
          "provider finality authority accepted");

  auto exact = ExactSet();
  refused = idx::UnionCandidateSets(left, exact, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "mixed_compressed_exact_algebra_unsupported",
          "mixed compressed/exact union did not fail closed");

  refused = idx::RerankCandidateSet(
      left,
      [](const idx::CandidateSetRow&) {
        return 1.0;
      },
      authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "compressed_bitmap_rerank_requires_exact_payload_stream",
          "compressed bitmap rerank did not require exact payload stream");

  refused = idx::ExactRecheckCandidateSet(left, authority);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "compressed_bitmap_exact_recheck_requires_row_locator_stream",
          "compressed bitmap exact recheck returned empty final rows");
}

}  // namespace

int main() {
  TestSparseRunDenseAndMixedAlgebra();
  TestComplementTopKAndPopcount();
  TestDeletedOverlayHandling();
  TestFailClosedContractsAndCorruption();
  std::cout << "compressed_bitmap_algebra_gate=passed\n";
  return EXIT_SUCCESS;
}
