// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compressed_bitmap_spill.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "compressed_bitmap_spill_repair_simd_gate: " << message << '\n';
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

void RechecksumArtifact(std::vector<platform::byte>* artifact) {
  Require(artifact->size() >= sizeof(platform::u64),
          "artifact too small to rechecksum");
  const auto checksum =
      Fnv1a64(artifact->data(), artifact->size() - sizeof(platform::u64));
  platform::StoreLittle64(artifact->data() + artifact->size() -
                              sizeof(platform::u64),
                          checksum);
}

void PersistArtifact(const std::filesystem::path& path,
                     const std::vector<platform::byte>& artifact) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "could not open spill artifact for write");
  out.write(reinterpret_cast<const char*>(artifact.data()),
            static_cast<std::streamsize>(artifact.size()));
  Require(out.good(), "could not write spill artifact");
}

std::vector<platform::byte> ReadArtifact(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.good(), "could not open spill artifact for read");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

idx::CandidateSet BuildSourceSet() {
  std::vector<platform::u64> ordinals = {1, 10};
  AppendRange(&ordinals, 65536 + 100, 65536 + 200);
  AppendRange(&ordinals, 131072, 131072 + 5000);
  auto built =
      idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(ordinals,
                                                           Authority(), true);
  Require(built.ok(), "source compressed bitmap build refused");
  Require(built.output.rows.empty(), "source build materialized rows");
  return built.output;
}

bool SameContainerPayload(
    const idx::CandidateSetCompressedBitmapContainer& left,
    const idx::CandidateSetCompressedBitmapContainer& right) {
  return left.type == right.type &&
         left.base_row_ordinal == right.base_row_ordinal &&
         left.ordinal_span == right.ordinal_span &&
         left.cardinality == right.cardinality &&
         left.array_offsets == right.array_offsets &&
         left.runs.size() == right.runs.size() &&
         left.bitmap_words == right.bitmap_words &&
         std::equal(left.runs.begin(), left.runs.end(), right.runs.begin(),
                    [](const auto& a, const auto& b) {
                      return a.start_offset == b.start_offset &&
                             a.run_length == b.run_length;
                    });
}

void RequireEquivalentCompressedSet(const idx::CandidateSet& reopened,
                                    const idx::CandidateSet& source) {
  Require(reopened.rows.empty(), "reopen materialized row vector");
  Require(reopened.encoding == idx::CandidateSetEncoding::compressed_bitmap,
          "reopen changed encoding");
  Require(reopened.compressed_bitmap_cardinality ==
              source.compressed_bitmap_cardinality,
          "reopen cardinality drift");
  Require(reopened.compressed_bitmap_min_row_ordinal ==
              source.compressed_bitmap_min_row_ordinal,
          "reopen min ordinal drift");
  Require(reopened.compressed_bitmap_max_row_ordinal ==
              source.compressed_bitmap_max_row_ordinal,
          "reopen max ordinal drift");
  Require(reopened.deleted_overlay_present == source.deleted_overlay_present,
          "reopen deleted overlay drift");
  Require(reopened.compressed_bitmap_containers.size() ==
              source.compressed_bitmap_containers.size(),
          "reopen container count drift");
  for (std::size_t i = 0; i < source.compressed_bitmap_containers.size(); ++i) {
    Require(SameContainerPayload(reopened.compressed_bitmap_containers[i],
                                 source.compressed_bitmap_containers[i]),
            "reopen container payload drift");
  }
}

idx::CompressedBitmapRepairAdmission Admission(
    idx::CompressedBitmapSpillDescriptor descriptor) {
  idx::CompressedBitmapRepairAdmission admission;
  admission.repair_admitted = true;
  admission.descriptor_match_proven = true;
  admission.authoritative_rebuild_input_proven = true;
  admission.admitted_spill_epoch = descriptor.spill_epoch;
  admission.admitted_source_generation = descriptor.source_generation;
  admission.proof_detail = "test_authoritative_compressed_candidate_set";
  return admission;
}

void TestCleanPersistedReopenAndPopcount() {
  const auto source = BuildSourceSet();
  const idx::CompressedBitmapSpillDescriptor descriptor{72, 9001};
  auto serialized = idx::SerializeCompressedBitmapSpill(source, descriptor);
  Require(serialized.ok(), "spill serialization refused");
  Require(!serialized.artifact.empty(), "spill artifact missing");
  Require(HasEvidence(serialized.evidence,
                      "compressed_bitmap_spill.serialization=deterministic"),
          "spill serialization evidence missing");
  Require(HasEvidence(serialized.evidence,
                      "candidate_set_finality_authority=false"),
          "candidate-set non-authority evidence missing");
  Require(HasEvidence(serialized.evidence, "parser_or_donor_authority=false"),
          "parser/provider non-authority evidence missing");
  Require(HasEvidence(serialized.evidence,
                      "wal_recovery_or_finality_authority=false"),
          "WAL non-authority evidence missing");

  const auto path = std::filesystem::temp_directory_path() /
                    "scratchbird_compressed_bitmap_spill_repair_simd_gate.sbc";
  PersistArtifact(path, serialized.artifact);
  const auto persisted = ReadArtifact(path);
  std::filesystem::remove(path);

  auto reopened = idx::OpenCompressedBitmapSpill(persisted, descriptor,
                                                Authority());
  Require(reopened.ok(), "clean persisted reopen refused");
  Require(reopened.classification ==
              idx::CompressedBitmapSpillClassification::clean_reopen,
          "clean reopen classification mismatch");
  RequireEquivalentCompressedSet(reopened.output, source);
  Require(HasEvidence(reopened.evidence,
                      "compressed_bitmap_spill.payload_checksum=validated"),
          "payload checksum validation evidence missing");
  Require(HasEvidence(reopened.evidence,
                      "compressed_bitmap_spill.artifact_checksum=validated"),
          "artifact checksum validation evidence missing");
  Require(HasEvidence(reopened.evidence,
                      "compressed_bitmap.popcount.container_level=true"),
          "container-level popcount evidence missing");

  const auto popcount = idx::CountCompressedBitmapWithSelectedBackend(source);
  Require(popcount.ok(), "selected backend popcount refused");
  Require(popcount.cardinality == source.compressed_bitmap_cardinality,
          "selected backend popcount cardinality drift");
  Require(HasEvidence(popcount.evidence,
                      "compressed_bitmap.popcount.simd_available=true") ||
              (HasEvidence(popcount.evidence,
                           "compressed_bitmap.popcount.simd_available=false") &&
               HasEvidence(popcount.evidence,
                           "compressed_bitmap.popcount.fallback_backend=scalar")),
          "SIMD/fallback popcount evidence missing");
}

void TestCrashReopenClassificationAndRepair() {
  const auto source = BuildSourceSet();
  const idx::CompressedBitmapSpillDescriptor descriptor{72, 9001};
  const auto serialized = idx::SerializeCompressedBitmapSpill(source, descriptor);
  Require(serialized.ok(), "source spill serialization refused");

  auto truncated = serialized.artifact;
  truncated.resize(24);
  auto reopened = idx::OpenCompressedBitmapSpill(truncated, descriptor,
                                                Authority());
  Require(!reopened.ok() && reopened.fail_closed &&
              reopened.classification ==
                  idx::CompressedBitmapSpillClassification::truncated &&
              reopened.refusal_reasons.front() ==
                  "compressed_bitmap_spill_truncated_header",
          "truncated spill classification mismatch");

  auto bad_checksum = serialized.artifact;
  bad_checksum[64] ^= 0x5a;
  reopened = idx::OpenCompressedBitmapSpill(bad_checksum, descriptor,
                                           Authority());
  Require(!reopened.ok() && reopened.fail_closed &&
              reopened.classification ==
                  idx::CompressedBitmapSpillClassification::corrupt &&
              reopened.refusal_reasons.front() ==
                  "compressed_bitmap_spill_artifact_checksum_mismatch",
          "bad checksum classification mismatch");

  auto stale_version = serialized.artifact;
  platform::StoreLittle16(stale_version.data() + 8, 99);
  RechecksumArtifact(&stale_version);
  reopened = idx::OpenCompressedBitmapSpill(stale_version, descriptor,
                                           Authority());
  Require(!reopened.ok() && reopened.fail_closed &&
              reopened.classification ==
                  idx::CompressedBitmapSpillClassification::stale &&
              reopened.refusal_reasons.front() ==
                  "compressed_bitmap_spill_incompatible_version",
          "stale version classification mismatch");

  reopened = idx::OpenCompressedBitmapSpill(
      serialized.artifact,
      idx::CompressedBitmapSpillDescriptor{73, 9001}, Authority());
  Require(!reopened.ok() && reopened.fail_closed &&
              reopened.classification ==
                  idx::CompressedBitmapSpillClassification::stale &&
              reopened.refusal_reasons.front() ==
                  "compressed_bitmap_spill_stale_epoch_or_generation",
          "stale epoch classification mismatch");

  idx::CompressedBitmapRepairAdmission no_admission;
  auto repaired = idx::RepairOrOpenCompressedBitmapSpill(
      bad_checksum, descriptor, Authority(), &source, no_admission);
  Require(!repaired.ok() && repaired.fail_closed &&
              repaired.classification ==
                  idx::CompressedBitmapSpillClassification::repair_refused &&
              repaired.refusal_reasons.front() ==
                  "compressed_bitmap_spill_repair_admission_not_proven",
          "repair without admission did not fail closed");

  repaired = idx::RepairOrOpenCompressedBitmapSpill(
      bad_checksum, descriptor, Authority(), nullptr, Admission(descriptor));
  Require(!repaired.ok() && repaired.fail_closed &&
              repaired.classification ==
                  idx::CompressedBitmapSpillClassification::repair_refused &&
              repaired.refusal_reasons.front() ==
                  "compressed_bitmap_spill_missing_authoritative_rebuild_input",
          "repair without authoritative input did not fail closed");

  repaired = idx::RepairOrOpenCompressedBitmapSpill(
      bad_checksum, descriptor, Authority(), &source, Admission(descriptor));
  Require(repaired.ok(), "admitted repair reopen refused");
  Require(repaired.classification ==
              idx::CompressedBitmapSpillClassification::repaired_reopen,
          "repaired reopen classification mismatch");
  RequireEquivalentCompressedSet(repaired.output, source);
  Require(HasEvidence(repaired.evidence,
                      "compressed_bitmap_spill.repair.admitted=true"),
          "repair admission evidence missing");
  Require(HasEvidence(
              repaired.evidence,
              "compressed_bitmap_spill.repair.authoritative_rebuild_input=true"),
          "authoritative rebuild input evidence missing");
  Require(HasEvidence(repaired.evidence,
                      "compressed_bitmap_spill.repair.non_authoritative=true"),
          "repair non-authority evidence missing");
  Require(HasEvidence(repaired.evidence,
                      "compressed_bitmap_spill.original_classification=corrupt"),
          "repair original classification evidence missing");
}

void TestAuthorityFailClosed() {
  const auto source = BuildSourceSet();
  const idx::CompressedBitmapSpillDescriptor descriptor{72, 9001};
  const auto serialized = idx::SerializeCompressedBitmapSpill(source, descriptor);
  Require(serialized.ok(), "authority source spill serialization refused");

  auto unsafe = Authority();
  unsafe.provider_finality_or_visibility_authority = true;
  auto reopened =
      idx::OpenCompressedBitmapSpill(serialized.artifact, descriptor, unsafe);
  Require(!reopened.ok() && reopened.fail_closed,
          "provider authority drift accepted");

  unsafe = Authority();
  unsafe.wal_recovery_or_finality_authority = true;
  reopened = idx::OpenCompressedBitmapSpill(serialized.artifact, descriptor,
                                           unsafe);
  Require(!reopened.ok() && reopened.fail_closed,
          "WAL authority drift accepted");

  auto corrupt = source;
  corrupt.compressed_bitmap_containers.front().ordinal_span = 1;
  auto popcount = idx::CountCompressedBitmapWithSelectedBackend(corrupt);
  Require(!popcount.ok() && popcount.fail_closed &&
              popcount.refusal_reasons.front() ==
                  "compressed_bitmap_popcount_invalid_candidate_set",
          "popcount accepted out-of-span compressed bitmap offsets");

  corrupt = source;
  corrupt.rows.push_back({});
  auto refused = idx::SerializeCompressedBitmapSpill(corrupt, descriptor);
  Require(!refused.ok() && refused.fail_closed &&
              refused.refusal_reasons.front() ==
                  "compressed_bitmap_popcount_invalid_candidate_set",
          "spill serialization accepted materialized rows in compressed set");
}

}  // namespace

int main() {
  TestCleanPersistedReopenAndPopcount();
  TestCrashReopenClassificationAndRepair();
  TestAuthorityFailClosed();
  std::cout << "compressed_bitmap_spill_repair_simd_gate=passed\n";
  return EXIT_SUCCESS;
}
