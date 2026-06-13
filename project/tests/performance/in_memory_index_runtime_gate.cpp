// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "in_memory_index_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "in_memory_index_runtime_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
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

idx::InMemoryIndexAuthorityProof Proof(platform::u64 epoch = 151) {
  idx::InMemoryIndexAuthorityProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.exact_source_available = true;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_recheck_required = true;
  proof.security_context_bound = true;
  proof.snapshot_proof_supplied = true;
  proof.snapshot_still_valid = true;
  proof.runtime_epoch = epoch;
  proof.mga_snapshot_epoch = 10;
  proof.catalog_snapshot_epoch = 20;
  proof.security_snapshot_epoch = 30;
  proof.evidence_token = "executor_exact_mga_security_snapshot_recheck";
  return proof;
}

idx::InMemoryIndexRuntimeState Runtime(platform::u64 epoch = 151,
                                       platform::u64 quota = 20000) {
  idx::InMemoryIndexRuntimeOptions options;
  options.runtime_epoch = epoch;
  options.memory_quota_bytes = quota;
  options.relation_uuid = "rel-irc-151";
  options.index_uuid = "idx-irc-151";
  return idx::CreateInMemoryIndexRuntime(std::move(options));
}

std::vector<idx::InMemoryIndexEntry> Entries() {
  return {{"alpha:001", "payload-a1", 1, "source-a1"},
          {"alpha:002", "payload-a2", 2, "source-a2"},
          {"beta:001", "payload-b1", 3, "source-b1"},
          {"beta:002", "payload-b2", 4, "source-b2"},
          {"gamma:001", "payload-g1", 5, "source-g1"}};
}

idx::InMemoryIndexColdSourceDescriptor ColdSource() {
  idx::InMemoryIndexColdSourceDescriptor descriptor;
  descriptor.relation_uuid = "rel-irc-151";
  descriptor.index_uuid = "idx-irc-151";
  descriptor.descriptor_epoch = 401;
  descriptor.persisted_generation = 902;
  descriptor.cold_source_supplied = true;
  descriptor.deterministic_order = true;
  descriptor.candidate_entries_only = true;
  descriptor.exact_recheck_required = true;
  descriptor.mga_recheck_required = true;
  descriptor.security_recheck_required = true;
  descriptor.entries = Entries();
  std::reverse(descriptor.entries.begin(), descriptor.entries.end());
  return descriptor;
}

idx::InMemoryIndexLookupRequest PointRequest(std::string key,
                                             platform::u64 epoch = 151) {
  idx::InMemoryIndexLookupRequest request;
  request.mode = idx::InMemoryIndexLookupMode::point;
  request.key = std::move(key);
  request.runtime_epoch = epoch;
  request.proof = Proof(epoch);
  return request;
}

void RequireCandidateOnlyEvidence(const std::vector<std::string>& evidence) {
  Require(HasEvidence(evidence, "in_memory.candidate_entries_only=true"),
          "candidate-only evidence missing");
  Require(HasEvidence(evidence, "in_memory.final_rows_authorized=false"),
          "final-row refusal evidence missing");
  Require(HasEvidence(evidence, "in_memory.exact_recheck.required=true"),
          "exact recheck evidence missing");
  Require(HasEvidence(evidence, "in_memory.mga_recheck.required=true"),
          "MGA recheck evidence missing");
  Require(HasEvidence(evidence, "in_memory.security_recheck.required=true"),
          "security recheck evidence missing");
  Require(HasEvidence(evidence, "in_memory.visibility_authority=false"),
          "visibility authority refusal evidence missing");
  Require(HasEvidence(evidence, "in_memory.index_authority=false"),
          "index authority refusal evidence missing");
  RequireNoRuntimeLeak(evidence);
}

void VerifyWarmColdRebuildAndQuota() {
  auto runtime = Runtime();
  auto rebuilt =
      idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(), Proof());
  Require(rebuilt.ok(), "warm/cold rebuild refused");
  Require(rebuilt.generation &&
              rebuilt.generation->generation_id == 1 &&
              rebuilt.generation->runtime_epoch == 151 &&
              runtime.current_memory_bytes > 0 &&
              runtime.peak_memory_bytes == runtime.current_memory_bytes &&
              runtime.total_rebuilds == 1,
          "warm/cold rebuild did not publish accounted generation");
  Require(HasEvidence(rebuilt.evidence, "in_memory.cold_rebuild=true"),
          "cold rebuild evidence missing");
  Require(HasEvidence(rebuilt.evidence,
                      "in_memory.warm_reopen_from_cold_source=true"),
          "warm reopen evidence missing");
  Require(HasEvidence(rebuilt.evidence,
                      "in_memory.immutable_generation=true"),
          "immutable generation evidence missing");
  RequireCandidateOnlyEvidence(rebuilt.evidence);

  auto denied_runtime = Runtime(151, 32);
  auto denied = idx::RebuildInMemoryIndexFromColdSource(
      &denied_runtime, ColdSource(), Proof());
  Require(!denied.ok() &&
              denied.open_class ==
                  idx::InMemoryIndexOpenClass::memory_quota_denied &&
              denied_runtime.total_denied_bytes > 0 &&
              denied.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.MEMORY_QUOTA_DENIED",
          "quota denial did not fail closed");
}

void VerifyLookupModesAndSnapshotAdmission() {
  auto runtime = Runtime();
  Require(idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                  Proof())
              .ok(),
          "lookup setup rebuild refused");

  auto point = idx::LookupInMemoryIndex(&runtime, PointRequest("beta:001"));
  Require(point.ok() && point.candidates.size() == 1 &&
              point.candidates.front().payload == "payload-b1",
          "point lookup did not return expected candidate");
  Require(HasEvidence(point.evidence,
                      "in_memory.lookup.immutable_snapshot_read=true"),
          "immutable snapshot read evidence missing");
  Require(HasEvidence(point.evidence,
                      "in_memory.lookup.mutable_shared_read=false"),
          "mutable shared read refusal evidence missing");
  RequireCandidateOnlyEvidence(point.evidence);

  idx::InMemoryIndexLookupRequest range;
  range.mode = idx::InMemoryIndexLookupMode::range;
  range.lower_key = "alpha:002";
  range.upper_key = "beta:002";
  range.runtime_epoch = 151;
  range.proof = Proof();
  auto range_result = idx::LookupInMemoryIndex(&runtime, range);
  Require(range_result.ok() && range_result.candidates.size() == 3,
          "range lookup candidate count drift");

  idx::InMemoryIndexLookupRequest prefix;
  prefix.mode = idx::InMemoryIndexLookupMode::prefix;
  prefix.prefix = "alpha:";
  prefix.runtime_epoch = 151;
  prefix.proof = Proof();
  auto prefix_result = idx::LookupInMemoryIndex(&runtime, prefix);
  Require(prefix_result.ok() && prefix_result.candidates.size() == 2,
          "prefix lookup candidate count drift");

  auto missing_snapshot = PointRequest("alpha:001");
  missing_snapshot.proof.snapshot_still_valid = false;
  auto refused = idx::LookupInMemoryIndex(&runtime, missing_snapshot);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::missing_recheck_proof &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.MISSING_RECHECK_PROOF",
          "missing snapshot proof did not fail closed");

  auto stale = idx::LookupInMemoryIndex(&runtime, PointRequest("alpha:001", 152));
  Require(!stale.ok() &&
              stale.open_class ==
                  idx::InMemoryIndexOpenClass::stale_runtime_epoch &&
              stale.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
          "stale runtime epoch did not fail closed");
}

void VerifyGenerationPublicationByMutations() {
  auto runtime = Runtime();
  Require(idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                  Proof())
              .ok(),
          "mutation setup rebuild refused");
  auto before = idx::LookupInMemoryIndex(&runtime, PointRequest("delta:001"));
  Require(before.ok() && before.candidates.empty(),
          "pre-insert lookup unexpectedly found candidate");

  idx::InMemoryIndexMutation insert;
  insert.kind = idx::InMemoryIndexMutationKind::insert_entry;
  insert.entry = {"delta:001", "payload-d1", 6, "source-d1"};
  insert.runtime_epoch = 151;
  insert.proof = Proof();
  auto inserted = idx::ApplyInMemoryIndexMutation(&runtime, insert);
  Require(inserted.ok() && inserted.generation->generation_id == 2 &&
              HasEvidence(inserted.evidence,
                          "in_memory.mutation.copy_on_write=true") &&
              HasEvidence(inserted.evidence,
                          "in_memory.mutation.published_new_generation=true"),
          "insert did not publish copy-on-write generation");

  auto after_insert =
      idx::LookupInMemoryIndex(&runtime, PointRequest("delta:001"));
  Require(after_insert.ok() && after_insert.candidates.size() == 1 &&
              after_insert.candidates.front().payload == "payload-d1",
          "inserted candidate was not visible in new generation");

  idx::InMemoryIndexMutation update = insert;
  update.kind = idx::InMemoryIndexMutationKind::update_entry;
  update.replacement_payload = "payload-d1-updated";
  update.replacement_exact_source_token = "source-d1-updated";
  auto updated = idx::ApplyInMemoryIndexMutation(&runtime, update);
  Require(updated.ok() && updated.generation->generation_id == 3,
          "update did not publish new generation");
  auto after_update =
      idx::LookupInMemoryIndex(&runtime, PointRequest("delta:001"));
  Require(after_update.ok() &&
              after_update.candidates.front().payload == "payload-d1-updated",
          "updated candidate payload drift");

  idx::InMemoryIndexMutation erase = insert;
  erase.kind = idx::InMemoryIndexMutationKind::delete_entry;
  auto deleted = idx::ApplyInMemoryIndexMutation(&runtime, erase);
  Require(deleted.ok() && deleted.generation->generation_id == 4,
          "delete did not publish new generation");
  auto after_delete =
      idx::LookupInMemoryIndex(&runtime, PointRequest("delta:001"));
  Require(after_delete.ok() && after_delete.candidates.empty(),
          "deleted candidate remained visible");
  Require(runtime.total_mutations == 3,
          "mutation counter did not track published generations");
}

void VerifyDuplicateIdentityRefusal() {
  auto runtime = Runtime();
  auto duplicate_source = ColdSource();
  duplicate_source.entries.push_back(
      {"alpha:001", "payload-duplicate", 1, "source-duplicate"});
  auto refused = idx::RebuildInMemoryIndexFromColdSource(
      &runtime, duplicate_source, Proof());
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::corrupt_cold_source &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.CORRUPT_COLD_SOURCE",
          "duplicate cold-source row locator did not fail closed");

  Require(idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                  Proof())
              .ok(),
          "duplicate mutation setup rebuild refused");
  const auto before = idx::BuildInMemoryIndexSupportBundle(&runtime);
  idx::InMemoryIndexMutation duplicate_insert;
  duplicate_insert.kind = idx::InMemoryIndexMutationKind::insert_entry;
  duplicate_insert.entry = {"alpha:001", "payload-new", 1, "source-new"};
  duplicate_insert.runtime_epoch = 151;
  duplicate_insert.proof = Proof();
  refused = idx::ApplyInMemoryIndexMutation(&runtime, duplicate_insert);
  Require(!refused.ok() &&
              refused.open_class == idx::InMemoryIndexOpenClass::refused &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.DUPLICATE_ENTRY_REFUSED",
          "duplicate mutation row locator did not fail closed");
  const auto after = idx::BuildInMemoryIndexSupportBundle(&runtime);
  Require(after.ok() && before.ok() &&
              after.generation_id == before.generation_id &&
              runtime.total_mutations == 0,
          "duplicate mutation changed the published generation");
}

void VerifyQuotaDeniedMutationKeepsPublishedGeneration() {
  auto runtime = Runtime();
  Require(idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                  Proof())
              .ok(),
          "quota mutation setup rebuild refused");
  const auto before = idx::BuildInMemoryIndexSupportBundle(&runtime);
  runtime.options.memory_quota_bytes = runtime.current_memory_bytes + 128;

  idx::InMemoryIndexMutation large_insert;
  large_insert.kind = idx::InMemoryIndexMutationKind::insert_entry;
  large_insert.entry = {"zeta:huge", std::string(4096, 'x'), 99,
                        "source-huge"};
  large_insert.runtime_epoch = 151;
  large_insert.proof = Proof();
  auto denied = idx::ApplyInMemoryIndexMutation(&runtime, large_insert);
  Require(!denied.ok() &&
              denied.open_class ==
                  idx::InMemoryIndexOpenClass::memory_quota_denied &&
              denied.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.MEMORY_QUOTA_DENIED",
          "quota-denied mutation did not fail closed");

  const auto after = idx::BuildInMemoryIndexSupportBundle(&runtime);
  auto lookup = idx::LookupInMemoryIndex(&runtime, PointRequest("zeta:huge"));
  Require(before.ok() && after.ok() &&
              after.generation_id == before.generation_id &&
              runtime.total_mutations == 0 &&
              lookup.ok() && lookup.candidates.empty(),
          "quota-denied mutation published or exposed candidates");
}

void VerifyStaleGenerationSupportFailsClosed() {
  auto runtime = Runtime();
  auto rebuilt =
      idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(), Proof());
  Require(rebuilt.ok() && rebuilt.generation,
          "stale support setup rebuild refused");
  auto stale =
      std::make_shared<idx::InMemoryIndexGeneration>(*rebuilt.generation);
  stale->runtime_epoch = 999;
  runtime.generation = stale;

  auto bundle = idx::BuildInMemoryIndexSupportBundle(&runtime);
  Require(!bundle.ok() &&
              bundle.fail_closed &&
              bundle.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
          "support bundle accepted stale in-memory generation");
}

void VerifyCleanupAndSupportBundle() {
  auto runtime = Runtime();
  Require(idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                  Proof())
              .ok(),
          "support setup rebuild refused");
  auto bundle = idx::BuildInMemoryIndexSupportBundle(&runtime);
  Require(bundle.ok() && !bundle.dropped && bundle.entry_count == 5 &&
              bundle.current_memory_bytes == runtime.current_memory_bytes &&
              HasEvidence(bundle.evidence, "in_memory.support_bundle=true"),
          "support bundle state drift");
  RequireCandidateOnlyEvidence(bundle.evidence);

  auto dropped = idx::DropInMemoryIndexRuntime(&runtime);
  Require(dropped.ok() && dropped.dropped &&
              runtime.current_memory_bytes == 0 &&
              runtime.dropped &&
              HasEvidence(dropped.evidence, "in_memory.drop.cleaned=true"),
          "drop did not cleanup runtime");
  RequireCandidateOnlyEvidence(dropped.evidence);

  auto refused = idx::LookupInMemoryIndex(&runtime, PointRequest("alpha:001"));
  Require(!refused.ok() &&
              refused.open_class == idx::InMemoryIndexOpenClass::dropped &&
              refused.diagnostic.diagnostic_code == "INDEX.IN_MEMORY.DROPPED",
          "dropped runtime admitted lookup");
}

void VerifyAuthorityAndFallbackRefusals() {
  auto runtime = Runtime();
  auto missing = Proof();
  missing.exact_source_available = false;
  auto refused =
      idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(), missing);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::missing_recheck_proof,
          "missing exact source proof did not fail closed");

  auto authority = Proof();
  authority.index_finality_authority_claimed = true;
  refused = idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(),
                                                    authority);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::authority_claim_refused &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.AUTHORITY_CLAIM_REFUSED",
          "authority claim did not fail closed");

  auto fallback = Proof();
  fallback.provider_only_fallback = true;
  refused =
      idx::RebuildInMemoryIndexFromColdSource(&runtime, ColdSource(), fallback);
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::unsafe_fallback_refused &&
              refused.diagnostic.diagnostic_code ==
                  "INDEX.IN_MEMORY.UNSAFE_FALLBACK_REFUSED",
          "provider-only fallback did not fail closed");

  auto descriptor_scan = ColdSource();
  descriptor_scan.descriptor_store_scan = true;
  refused = idx::RebuildInMemoryIndexFromColdSource(
      &runtime, descriptor_scan, Proof());
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::descriptor_scan_refused,
          "descriptor-store scan did not fail closed");

  auto behavior_scan = ColdSource();
  behavior_scan.behavior_store_scan = true;
  refused = idx::RebuildInMemoryIndexFromColdSource(&runtime, behavior_scan,
                                                    Proof());
  Require(!refused.ok() &&
              refused.open_class ==
                  idx::InMemoryIndexOpenClass::behavior_scan_refused,
          "behavior-store scan did not fail closed");
}

}  // namespace

int main() {
  VerifyWarmColdRebuildAndQuota();
  VerifyLookupModesAndSnapshotAdmission();
  VerifyGenerationPublicationByMutations();
  VerifyDuplicateIdentityRefusal();
  VerifyQuotaDeniedMutationKeepsPublishedGeneration();
  VerifyStaleGenerationSupportFailsClosed();
  VerifyCleanupAndSupportBundle();
  VerifyAuthorityAndFallbackRefusals();
  std::cout << "in_memory_index_runtime_gate=passed\n";
  return 0;
}
