// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "temporary_work_index_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "temporary_work_index_runtime_gate: " << message << '\n';
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

void RequireNoRuntimeLeak(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    Require(item.find("docs/") == std::string::npos &&
                item.find("execution_plan") == std::string::npos &&
                item.find("contract") == std::string::npos &&
                item.find("IRC-") == std::string::npos,
            "runtime evidence leaked non-runtime artifact");
    if (item.find("reference") != std::string::npos) {
      Require(item.find("reference_authority=false") != std::string::npos,
              "runtime evidence leaked non-runtime reference artifact");
    }
  }
}

idx::TemporaryWorkAuthorityProof Proof() {
  idx::TemporaryWorkAuthorityProof proof;
  proof.proof_supplied = true;
  proof.exact_recheck_required = true;
  proof.exact_recheck_available = true;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_recheck_required = true;
  proof.security_context_bound = true;
  proof.evidence_ref = "executor_exact_mga_security_recheck";
  return proof;
}

idx::CandidateSetAuthorityContext CandidateAuthority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

idx::TemporaryWorkRuntimeState Runtime(const std::filesystem::path& root,
                                       platform::u64 generation,
                                       platform::u64 quota) {
  idx::TemporaryWorkRuntimeOptions options;
  options.spill_directory = root;
  options.runtime_generation = generation;
  options.memory_quota_bytes = quota;
  options.artifact_prefix = "temporary_work_index_runtime_gate";
  return idx::CreateTemporaryWorkRuntime(std::move(options));
}

std::vector<idx::TemporaryWorkRecord> SortRows() {
  std::vector<idx::TemporaryWorkRecord> rows;
  for (platform::u64 i = 0; i < 24; ++i) {
    rows.push_back({"k" + std::to_string(24 - i),
                    std::string(80, static_cast<char>('a' + (i % 20))),
                    i + 1});
  }
  return rows;
}

std::vector<idx::TemporaryHashBuildRow> HashRows() {
  std::vector<idx::TemporaryHashBuildRow> rows;
  for (platform::u64 i = 0; i < 20; ++i) {
    rows.push_back({"join_key_" + std::to_string(i % 5),
                    std::string(64, static_cast<char>('A' + (i % 20))),
                    i + 100});
  }
  return rows;
}

std::vector<idx::TemporaryBulkSortBufferEntry> BulkEntries() {
  std::vector<idx::TemporaryBulkSortBufferEntry> entries;
  for (platform::u64 i = 0; i < 18; ++i) {
    entries.push_back({"bulk_key_" + std::to_string(18 - i),
                       std::string(72, static_cast<char>('f' + (i % 10))),
                       i});
  }
  return entries;
}

idx::CandidateSet BitmapSet() {
  std::vector<platform::u64> ordinals;
  for (platform::u64 i = 10; i < 40; ++i) {
    ordinals.push_back(i);
  }
  for (platform::u64 i = 65536 + 7; i < 65536 + 90; ++i) {
    ordinals.push_back(i);
  }
  for (platform::u64 i = 131072 + 200; i < 131072 + 240; ++i) {
    ordinals.push_back(i);
  }
  const auto built = idx::MakeCompressedBitmapCandidateSetFromRowOrdinals(
      ordinals, CandidateAuthority(), true);
  Require(built.ok(), "compressed bitmap candidate set refused");
  Require(built.output.rows.empty(), "bitmap candidate set materialized rows");
  return built.output;
}

void RequireDescriptorCandidateOnly(
    const idx::TemporaryWorkArtifactDescriptor& descriptor) {
  Require(descriptor.candidate_rows_only &&
              descriptor.exact_recheck_required &&
              descriptor.mga_recheck_required &&
              descriptor.security_recheck_required,
          "descriptor omitted candidate-only recheck proof");
  Require(HasEvidence(descriptor.evidence,
                      "temporary_work.candidate_rows_only=true"),
          "candidate-only evidence missing");
  Require(HasEvidence(descriptor.evidence,
                      "temporary_work.final_rows_authorized=false"),
          "final-row refusal evidence missing");
  RequireNoRuntimeLeak(descriptor.evidence);
}

void OverwriteFile(const std::filesystem::path& path,
                   const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not overwrite temporary artifact");
}

void RequireSpilledCurrent(const idx::TemporaryWorkResult& result,
                           idx::TemporaryWorkFamily family) {
  Require(result.ok(), "temporary work build/open refused");
  Require(result.open_class == idx::TemporaryWorkOpenClass::current,
          "temporary work open class not current");
  Require(result.descriptor.family == family, "temporary family drift");
  Require(result.descriptor.spilled, "bounded quota did not create spill");
  Require(std::filesystem::exists(result.descriptor.path),
          "spill artifact file missing");
  RequireDescriptorCandidateOnly(result.descriptor);
  RequireNoRuntimeLeak(result.evidence);
}

void VerifyInMemoryArtifactLifecycle(const std::filesystem::path& root) {
  auto runtime = Runtime(root / "memory", 149, 1000000);
  auto built = idx::BuildTemporarySortRun(&runtime, SortRows(), Proof());
  Require(built.ok() &&
              !built.descriptor.spilled &&
              built.descriptor.path.empty() &&
              !built.descriptor.artifact.empty() &&
              !HasEvidence(built.evidence, "temporary_work.spill_created=true"),
          "in-memory temporary work incorrectly created a spill artifact");
  RequireDescriptorCandidateOnly(built.descriptor);

  auto reopened = idx::OpenTemporaryWorkArtifact(
      &runtime, built.descriptor, idx::TemporaryWorkFamily::sort_run, Proof());
  Require(reopened.ok() &&
              reopened.sorted_rows.size() == built.sorted_rows.size(),
          "in-memory temporary artifact did not reopen from descriptor bytes");

  auto forged = built.descriptor;
  forged.memory_grant_bytes += 1;
  const auto forged_open = idx::OpenTemporaryWorkArtifact(
      &runtime, forged, idx::TemporaryWorkFamily::sort_run, Proof());
  Require(!forged_open.ok() &&
              forged_open.open_class ==
                  idx::TemporaryWorkOpenClass::corrupt_spill_payload,
          "temporary work trusted forged descriptor grant metadata");

  auto cleanup =
      idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
  Require(cleanup.ok() &&
              runtime.live_granted_bytes == 0 &&
              cleanup.released_grant_bytes == built.descriptor.memory_grant_bytes,
          "in-memory temporary cleanup did not release grant");
}

void VerifySortRunLifecycle(const std::filesystem::path& root) {
  auto runtime = Runtime(root / "sort", 150, 700);
  auto built = idx::BuildTemporarySortRun(&runtime, SortRows(), Proof());
  RequireSpilledCurrent(built, idx::TemporaryWorkFamily::sort_run);
  Require(std::is_sorted(built.sorted_rows.begin(), built.sorted_rows.end(),
                         [](const auto& left, const auto& right) {
                           return left.key < right.key ||
                                  (left.key == right.key &&
                                   left.row_ordinal < right.row_ordinal);
                         }),
          "sort run was not physically sorted");

  auto reopened = idx::OpenTemporaryWorkArtifact(
      &runtime, built.descriptor, idx::TemporaryWorkFamily::sort_run, Proof());
  Require(reopened.ok(), "sort run reopen refused");
  Require(reopened.sorted_rows.size() == built.sorted_rows.size(),
          "sort run reopen row count drift");
  Require(HasEvidence(reopened.evidence,
                      "temporary_work.spill_payload_checksum=validated"),
          "sort run reopen checksum evidence missing");

  auto cleanup =
      idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
  Require(cleanup.ok() && cleanup.cleaned, "sort run cleanup refused");
  Require(runtime.live_granted_bytes == 0,
          "sort cleanup did not release grant");
  Require(!std::filesystem::exists(built.descriptor.path),
          "sort cleanup left spill file");

  auto stale = idx::OpenTemporaryWorkArtifact(
      &runtime, built.descriptor, idx::TemporaryWorkFamily::sort_run, Proof());
  Require(!stale.ok() && stale.open_class ==
                            idx::TemporaryWorkOpenClass::cleaned_artifact,
          "cleaned sort artifact reopened as stale candidate");
}

void VerifyHashBitmapAndBulk(const std::filesystem::path& root) {
  auto runtime = Runtime(root / "multi", 151, 400);

  auto hash = idx::BuildTemporaryHashJoinTable(&runtime, HashRows(), Proof());
  RequireSpilledCurrent(hash,
                        idx::TemporaryWorkFamily::hash_join_build_table);
  auto hash_reopen = idx::OpenTemporaryWorkArtifact(
      &runtime, hash.descriptor,
      idx::TemporaryWorkFamily::hash_join_build_table, Proof());
  Require(hash_reopen.ok() &&
              hash_reopen.hash_build_rows.size() ==
                  hash.hash_build_rows.size(),
          "hash join build table reopen drift");
  Require(idx::CleanupTemporaryWorkArtifact(&runtime,
                                            hash.descriptor.artifact_id).ok(),
          "hash cleanup refused");

  auto bitmap = idx::BuildTemporaryBitmapCandidateSet(&runtime, BitmapSet(),
                                                      Proof());
  RequireSpilledCurrent(
      bitmap, idx::TemporaryWorkFamily::temporary_bitmap_candidate_set);
  Require(bitmap.bitmap_candidate_set.rows.empty() &&
              !bitmap.bitmap_candidate_set.final_rows_authorized &&
              bitmap.bitmap_candidate_set.requires_exact_recheck &&
              bitmap.bitmap_candidate_set.requires_mga_visibility_recheck &&
              bitmap.bitmap_candidate_set
                  .requires_security_authorization_recheck,
          "bitmap result was not candidate-only evidence");
  auto bitmap_reopen = idx::OpenTemporaryWorkArtifact(
      &runtime, bitmap.descriptor,
      idx::TemporaryWorkFamily::temporary_bitmap_candidate_set, Proof());
  Require(bitmap_reopen.ok() &&
              bitmap_reopen.bitmap_candidate_set
                      .compressed_bitmap_cardinality ==
                  bitmap.bitmap_candidate_set.compressed_bitmap_cardinality,
          "bitmap candidate set reopen drift");
  Require(idx::CleanupTemporaryWorkArtifact(
              &runtime, bitmap.descriptor.artifact_id)
              .ok(),
          "bitmap cleanup refused");

  auto bulk = idx::BuildTemporaryBulkSortBuffer(&runtime, BulkEntries(),
                                                Proof());
  RequireSpilledCurrent(bulk, idx::TemporaryWorkFamily::bulk_sort_buffer);
  auto bulk_reopen = idx::OpenTemporaryWorkArtifact(
      &runtime, bulk.descriptor,
      idx::TemporaryWorkFamily::bulk_sort_buffer, Proof());
  Require(bulk_reopen.ok() &&
              bulk_reopen.bulk_sort_buffer.size() ==
                  bulk.bulk_sort_buffer.size(),
          "bulk sort buffer reopen drift");
  Require(idx::CleanupTemporaryWorkArtifact(&runtime,
                                            bulk.descriptor.artifact_id).ok(),
          "bulk cleanup refused");
  Require(runtime.live_granted_bytes == 0,
          "multi-family cleanup left memory grants");
}

void VerifyRefusals(const std::filesystem::path& root) {
  auto runtime = Runtime(root / "refusal", 152, 2048);
  auto missing = Proof();
  missing.exact_recheck_available = false;
  auto refused =
      idx::BuildTemporarySortRun(&runtime, SortRows(), missing);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::missing_recheck_proof &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.MISSING_RECHECK_PROOF",
          "missing exact proof did not fail closed");

  missing = Proof();
  missing.mga_visibility_recheck_available = false;
  refused = idx::BuildTemporarySortRun(&runtime, SortRows(), missing);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::missing_recheck_proof &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.MISSING_RECHECK_PROOF",
          "missing MGA proof did not fail closed");

  missing = Proof();
  missing.security_context_bound = false;
  refused = idx::BuildTemporarySortRun(&runtime, SortRows(), missing);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::missing_recheck_proof &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.MISSING_RECHECK_PROOF",
          "missing security proof did not fail closed");

  auto fallback = Proof();
  fallback.contract_only_fallback = true;
  refused = idx::BuildTemporarySortRun(&runtime, SortRows(), fallback);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::unsafe_fallback_refused &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.UNSAFE_FALLBACK_REFUSED",
          "unsafe fallback did not fail closed");

  auto authority = Proof();
  authority.index_finality_authority_claimed = true;
  refused = idx::BuildTemporarySortRun(&runtime, SortRows(), authority);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::authority_claim_refused &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.AUTHORITY_CLAIM_REFUSED",
          "authority claim did not fail closed");

  auto denied_runtime = Runtime(root / "denial", 153, 16);
  refused = idx::BuildTemporarySortRun(&denied_runtime, SortRows(), Proof(),
                                       false);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::TemporaryWorkOpenClass::memory_grant_denied &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.MEMORY_GRANT_DENIED" &&
              denied_runtime.total_denied_bytes > 0,
          "memory grant denial did not fail closed");
}

void VerifyStaleCorruptAndCancel(const std::filesystem::path& root) {
  auto runtime = Runtime(root / "stale", 154, 700);
  auto built = idx::BuildTemporarySortRun(&runtime, SortRows(), Proof());
  RequireSpilledCurrent(built, idx::TemporaryWorkFamily::sort_run);

  auto newer_runtime = Runtime(root / "stale", 155, 700);
  auto stale = idx::OpenTemporaryWorkArtifact(
      &newer_runtime, built.descriptor,
      idx::TemporaryWorkFamily::sort_run, Proof());
  Require(!stale.ok() &&
              stale.open_class ==
                  idx::TemporaryWorkOpenClass::stale_runtime_generation &&
              stale.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.STALE_RUNTIME_GENERATION",
          "stale runtime generation did not fail closed");

  auto corrupt_descriptor = built.descriptor;
  Require(corrupt_descriptor.artifact.size() > 90,
          "artifact too small for corruption check");
  corrupt_descriptor.artifact[90] ^= 0x5a;
  OverwriteFile(corrupt_descriptor.path, corrupt_descriptor.artifact);
  auto corrupt = idx::OpenTemporaryWorkArtifact(
      &runtime, corrupt_descriptor, idx::TemporaryWorkFamily::sort_run,
      Proof());
  Require(!corrupt.ok() &&
              corrupt.open_class ==
                  idx::TemporaryWorkOpenClass::corrupt_spill_payload &&
              corrupt.diagnostic.diagnostic_code ==
                  "INDEX.TEMPORARY_WORK.CORRUPT_SPILL_PAYLOAD",
          "corrupt spill payload did not fail closed");

  auto cancel_runtime = Runtime(root / "cancel", 156, 10000);
  auto sort =
      idx::BuildTemporarySortRun(&cancel_runtime, SortRows(), Proof());
  auto hash =
      idx::BuildTemporaryHashJoinTable(&cancel_runtime, HashRows(), Proof());
  Require(sort.ok() && hash.ok(), "cancel setup build refused");
  auto sort_path = sort.descriptor.path;
  auto hash_path = hash.descriptor.path;
  auto cancelled = idx::CancelTemporaryWorkRuntime(&cancel_runtime);
  Require(cancelled.ok() && cancelled.cleaned &&
              cancelled.cleaned_artifact_ids.size() == 2 &&
              cancel_runtime.active_artifacts.empty() &&
              cancel_runtime.live_granted_bytes == 0 &&
              cancel_runtime.cancelled,
          "cancel did not cleanup all temporary artifacts");
  Require(!std::filesystem::exists(sort_path) &&
              !std::filesystem::exists(hash_path),
          "cancel left spill file behind");
  auto after_cancel =
      idx::BuildTemporarySortRun(&cancel_runtime, SortRows(), Proof());
  Require(!after_cancel.ok() &&
              after_cancel.open_class ==
                  idx::TemporaryWorkOpenClass::cancelled,
          "cancelled runtime admitted new temporary work");
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_temporary_work_index_runtime_gate_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  VerifyInMemoryArtifactLifecycle(root);
  VerifySortRunLifecycle(root);
  VerifyHashBitmapAndBulk(root);
  VerifyRefusals(root);
  VerifyStaleCorruptAndCancel(root);
  std::filesystem::remove_all(root, ignored);
  std::cout << "temporary_work_index_runtime_gate=passed\n";
  return 0;
}
