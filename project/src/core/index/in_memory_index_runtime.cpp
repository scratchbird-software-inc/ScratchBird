// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "in_memory_index_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

inline constexpr u64 kEntryOverheadBytes = 72;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

Status MemoryDeniedStatus() {
  return {StatusCode::memory_limit_exceeded, Severity::error,
          Subsystem::memory};
}

bool AuthorityClaimed(const InMemoryIndexAuthorityProof& proof) {
  return proof.parser_finality_authority_claimed ||
         proof.donor_finality_authority_claimed ||
         proof.provider_finality_authority_claimed ||
         proof.index_finality_authority_claimed ||
         proof.transaction_finality_authority_claimed ||
         proof.recovery_finality_authority_claimed ||
         proof.visibility_authority_claimed ||
         proof.security_authority_claimed ||
         proof.log_finality_authority_claimed;
}

bool UnsafeFallbackClaimed(const InMemoryIndexAuthorityProof& proof) {
  return proof.contract_only_fallback ||
         proof.lifecycle_only_fallback ||
         proof.provider_only_fallback ||
         proof.unsafe_mutable_shared_read;
}

bool DescriptorAuthorityClaimed(
    const InMemoryIndexColdSourceDescriptor& descriptor) {
  return descriptor.parser_finality_authority_claimed ||
         descriptor.donor_finality_authority_claimed ||
         descriptor.provider_finality_authority_claimed ||
         descriptor.index_finality_authority_claimed ||
         descriptor.transaction_finality_authority_claimed ||
         descriptor.recovery_finality_authority_claimed ||
         descriptor.visibility_authority_claimed ||
         descriptor.security_authority_claimed ||
         descriptor.log_finality_authority_claimed;
}

bool DescriptorUnsafeFallbackClaimed(
    const InMemoryIndexColdSourceDescriptor& descriptor) {
  return descriptor.contract_only_fallback ||
         descriptor.lifecycle_only_fallback ||
         descriptor.provider_only_fallback;
}

bool RecheckProofValid(const InMemoryIndexAuthorityProof& proof,
                       u64 runtime_epoch) {
  return proof.proof_supplied &&
         proof.exact_source_recheck_required &&
         proof.exact_source_available &&
         proof.mga_visibility_recheck_required &&
         proof.mga_visibility_recheck_available &&
         proof.security_recheck_required &&
         proof.security_context_bound &&
         proof.snapshot_proof_supplied &&
         proof.snapshot_still_valid &&
         proof.runtime_epoch == runtime_epoch &&
         proof.mga_snapshot_epoch > 0 &&
         proof.catalog_snapshot_epoch > 0 &&
         proof.security_snapshot_epoch > 0 &&
         !proof.evidence_token.empty() &&
         !AuthorityClaimed(proof) &&
         !UnsafeFallbackClaimed(proof);
}

bool DescriptorSafe(const InMemoryIndexRuntimeState& runtime,
                    const InMemoryIndexColdSourceDescriptor& descriptor) {
  return descriptor.cold_source_supplied &&
         descriptor.descriptor_epoch > 0 &&
         descriptor.persisted_generation > 0 &&
         descriptor.deterministic_order &&
         descriptor.candidate_entries_only &&
         descriptor.exact_recheck_required &&
         descriptor.mga_recheck_required &&
         descriptor.security_recheck_required &&
         descriptor.relation_uuid == runtime.options.relation_uuid &&
         descriptor.index_uuid == runtime.options.index_uuid &&
         !descriptor.entries.empty() &&
         !descriptor.descriptor_store_scan &&
         !descriptor.behavior_store_scan &&
         !DescriptorUnsafeFallbackClaimed(descriptor) &&
         !DescriptorAuthorityClaimed(descriptor);
}

std::vector<std::string> BaseEvidence() {
  return {std::string(kInMemoryIndexRuntimeSearchKey),
          "in_memory.family=in_memory",
          "in_memory.candidate_entries_only=true",
          "in_memory.final_rows_authorized=false",
          "in_memory.visibility_authority=false",
          "in_memory.security_authority=false",
          "in_memory.transaction_finality_authority=false",
          "in_memory.recovery_finality_authority=false",
          "in_memory.parser_or_donor_authority=false",
          "in_memory.provider_authority=false",
          "in_memory.index_authority=false",
          "in_memory.exact_recheck.required=true",
          "in_memory.mga_recheck.required=true",
          "in_memory.security_recheck.required=true"};
}

void AppendEvidence(std::vector<std::string>* target,
                    const std::vector<std::string>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

std::string CountEvidence(const char* key, u64 value) {
  return std::string(key) + "=" + std::to_string(value);
}

std::shared_ptr<const InMemoryIndexGeneration> LoadGeneration(
    const InMemoryIndexRuntimeState& runtime) {
  return std::atomic_load_explicit(&runtime.generation,
                                   std::memory_order_acquire);
}

void StoreGeneration(
    InMemoryIndexRuntimeState* runtime,
    std::shared_ptr<const InMemoryIndexGeneration> generation) {
  std::atomic_store_explicit(&runtime->generation, std::move(generation),
                             std::memory_order_release);
}

u64 EntryBytes(const InMemoryIndexEntry& entry) {
  return kEntryOverheadBytes + entry.key.size() + entry.payload.size() +
         entry.exact_source_token.size();
}

u64 EstimateBytes(
    const std::map<std::string, std::vector<InMemoryIndexEntry>>& entries) {
  u64 bytes = 0;
  for (const auto& [key, values] : entries) {
    bytes += kEntryOverheadBytes + key.size();
    for (const auto& entry : values) {
      bytes += EntryBytes(entry);
    }
  }
  return bytes;
}

u64 EntryCount(
    const std::map<std::string, std::vector<InMemoryIndexEntry>>& entries) {
  u64 count = 0;
  for (const auto& [key, values] : entries) {
    (void)key;
    count += static_cast<u64>(values.size());
  }
  return count;
}

bool EntryValid(const InMemoryIndexEntry& entry) {
  return !entry.key.empty() && entry.row_ordinal > 0 &&
         !entry.exact_source_token.empty();
}

bool EntryLess(const InMemoryIndexEntry& left,
               const InMemoryIndexEntry& right) {
  return std::tie(left.key, left.row_ordinal, left.payload,
                  left.exact_source_token) <
         std::tie(right.key, right.row_ordinal, right.payload,
                  right.exact_source_token);
}

bool SameEntryIdentity(const InMemoryIndexEntry& left,
                       const InMemoryIndexEntry& right) {
  return left.key == right.key && left.row_ordinal == right.row_ordinal;
}

std::map<std::string, std::vector<InMemoryIndexEntry>> CanonicalEntries(
    std::vector<InMemoryIndexEntry> entries,
    bool* valid) {
  *valid = true;
  std::sort(entries.begin(), entries.end(), EntryLess);
  std::map<std::string, std::vector<InMemoryIndexEntry>> by_key;
  bool have_last_identity = false;
  std::string last_key;
  u64 last_row_ordinal = 0;
  for (auto& entry : entries) {
    if (!EntryValid(entry)) {
      *valid = false;
      return {};
    }
    if (have_last_identity && entry.key == last_key &&
        entry.row_ordinal == last_row_ordinal) {
      *valid = false;
      return {};
    }
    last_key = entry.key;
    last_row_ordinal = entry.row_ordinal;
    have_last_identity = true;
    by_key[entry.key].push_back(std::move(entry));
  }
  return by_key;
}

InMemoryIndexResult Refuse(InMemoryIndexOpenClass open_class,
                           Status status,
                           std::string diagnostic_code,
                           std::string message_key,
                           std::string reason) {
  InMemoryIndexResult result;
  result.status = status;
  result.fail_closed = true;
  result.open_class = open_class;
  result.diagnostic = MakeInMemoryIndexRuntimeDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence();
  result.evidence.push_back(std::string("in_memory.open_class=") +
                            InMemoryIndexOpenClassName(open_class));
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

InMemoryIndexSupportBundle SupportRefuse(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string reason) {
  InMemoryIndexSupportBundle result;
  result.status = status;
  result.fail_closed = true;
  result.diagnostic = MakeInMemoryIndexRuntimeDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence();
  result.evidence.push_back("in_memory.support_bundle.fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

InMemoryIndexResult ValidateRuntimeAndProof(
    const InMemoryIndexRuntimeState* runtime,
    u64 request_epoch,
    const InMemoryIndexAuthorityProof& proof) {
  if (runtime == nullptr) {
    return Refuse(InMemoryIndexOpenClass::refused, ErrorStatus(),
                  "INDEX.IN_MEMORY.RUNTIME_MISSING",
                  "index.in_memory.runtime_missing",
                  "runtime_state_missing");
  }
  if (runtime->dropped) {
    return Refuse(InMemoryIndexOpenClass::dropped, ErrorStatus(),
                  "INDEX.IN_MEMORY.DROPPED",
                  "index.in_memory.dropped",
                  "in_memory_index_dropped");
  }
  if (request_epoch != runtime->options.runtime_epoch ||
      proof.runtime_epoch != runtime->options.runtime_epoch) {
    return Refuse(InMemoryIndexOpenClass::stale_runtime_epoch,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
                  "index.in_memory.stale_runtime_epoch",
                  "in_memory_runtime_epoch_stale");
  }
  if (AuthorityClaimed(proof)) {
    return Refuse(InMemoryIndexOpenClass::authority_claim_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.AUTHORITY_CLAIM_REFUSED",
                  "index.in_memory.authority_claim_refused",
                  "in_memory_cannot_own_visibility_security_or_finality");
  }
  if (UnsafeFallbackClaimed(proof)) {
    return Refuse(InMemoryIndexOpenClass::unsafe_fallback_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.UNSAFE_FALLBACK_REFUSED",
                  "index.in_memory.unsafe_fallback_refused",
                  "unsafe_in_memory_fallback_mode");
  }
  if (!RecheckProofValid(proof, runtime->options.runtime_epoch)) {
    return Refuse(InMemoryIndexOpenClass::missing_recheck_proof,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.MISSING_RECHECK_PROOF",
                  "index.in_memory.missing_recheck_proof",
                  "missing_exact_mga_security_or_snapshot_proof");
  }
  InMemoryIndexResult ok;
  ok.status = OkStatus();
  ok.open_class = InMemoryIndexOpenClass::current;
  return ok;
}

InMemoryIndexResult PublishGeneration(
    InMemoryIndexRuntimeState* runtime,
    std::map<std::string, std::vector<InMemoryIndexEntry>> entries,
    u64 source_descriptor_epoch,
    u64 persisted_generation,
    u64 next_generation_id,
    std::vector<std::string> extra_evidence) {
  const u64 estimated_bytes = EstimateBytes(entries);
  if (estimated_bytes > runtime->options.memory_quota_bytes) {
    runtime->total_denied_bytes += estimated_bytes;
    return Refuse(InMemoryIndexOpenClass::memory_quota_denied,
                  MemoryDeniedStatus(),
                  "INDEX.IN_MEMORY.MEMORY_QUOTA_DENIED",
                  "index.in_memory.memory_quota_denied",
                  "in_memory_memory_quota_denied");
  }

  auto generation = std::make_shared<InMemoryIndexGeneration>();
  generation->runtime_epoch = runtime->options.runtime_epoch;
  generation->generation_id = next_generation_id;
  generation->source_descriptor_epoch = source_descriptor_epoch;
  generation->persisted_generation = persisted_generation;
  generation->estimated_bytes = estimated_bytes;
  generation->entries_by_key = std::move(entries);
  generation->evidence = BaseEvidence();
  generation->evidence.push_back("in_memory.immutable_generation=true");
  generation->evidence.push_back("in_memory.read_copy_publish=true");
  generation->evidence.push_back("in_memory.mutable_shared_read=false");
  generation->evidence.push_back(
      CountEvidence("in_memory.runtime_epoch",
                    runtime->options.runtime_epoch));
  generation->evidence.push_back(
      CountEvidence("in_memory.generation_id",
                    generation->generation_id));
  generation->evidence.push_back(
      CountEvidence("in_memory.memory.estimated_bytes", estimated_bytes));
  generation->evidence.push_back(
      CountEvidence("in_memory.entry_count",
                    EntryCount(generation->entries_by_key)));
  AppendEvidence(&generation->evidence, extra_evidence);

  StoreGeneration(runtime, generation);
  runtime->current_memory_bytes = estimated_bytes;
  runtime->peak_memory_bytes =
      std::max(runtime->peak_memory_bytes, runtime->current_memory_bytes);

  InMemoryIndexResult result;
  result.status = OkStatus();
  result.open_class = InMemoryIndexOpenClass::current;
  result.generation = generation;
  result.evidence = generation->evidence;
  return result;
}

}  // namespace

InMemoryIndexRuntimeState CreateInMemoryIndexRuntime(
    InMemoryIndexRuntimeOptions options) {
  InMemoryIndexRuntimeState state;
  state.options = std::move(options);
  state.evidence = BaseEvidence();
  state.evidence.push_back("in_memory.runtime_created=true");
  state.evidence.push_back(
      CountEvidence("in_memory.runtime_epoch", state.options.runtime_epoch));
  state.evidence.push_back(
      CountEvidence("in_memory.memory.quota_bytes",
                    state.options.memory_quota_bytes));
  return state;
}

const char* InMemoryIndexOpenClassName(InMemoryIndexOpenClass open_class) {
  switch (open_class) {
    case InMemoryIndexOpenClass::current:
      return "current";
    case InMemoryIndexOpenClass::stale_runtime_epoch:
      return "stale_runtime_epoch";
    case InMemoryIndexOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case InMemoryIndexOpenClass::memory_quota_denied:
      return "memory_quota_denied";
    case InMemoryIndexOpenClass::corrupt_cold_source:
      return "corrupt_cold_source";
    case InMemoryIndexOpenClass::descriptor_scan_refused:
      return "descriptor_scan_refused";
    case InMemoryIndexOpenClass::behavior_scan_refused:
      return "behavior_scan_refused";
    case InMemoryIndexOpenClass::unsafe_fallback_refused:
      return "unsafe_fallback_refused";
    case InMemoryIndexOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case InMemoryIndexOpenClass::dropped:
      return "dropped";
    case InMemoryIndexOpenClass::refused:
      return "refused";
  }
  return "refused";
}

const char* InMemoryIndexLookupModeName(InMemoryIndexLookupMode mode) {
  switch (mode) {
    case InMemoryIndexLookupMode::point:
      return "point";
    case InMemoryIndexLookupMode::range:
      return "range";
    case InMemoryIndexLookupMode::prefix:
      return "prefix";
  }
  return "unknown";
}

const char* InMemoryIndexMutationKindName(InMemoryIndexMutationKind kind) {
  switch (kind) {
    case InMemoryIndexMutationKind::insert_entry:
      return "insert_entry";
    case InMemoryIndexMutationKind::update_entry:
      return "update_entry";
    case InMemoryIndexMutationKind::delete_entry:
      return "delete_entry";
  }
  return "unknown";
}

InMemoryIndexResult RebuildInMemoryIndexFromColdSource(
    InMemoryIndexRuntimeState* runtime,
    InMemoryIndexColdSourceDescriptor descriptor,
    const InMemoryIndexAuthorityProof& proof) {
  auto admission =
      ValidateRuntimeAndProof(runtime, proof.runtime_epoch, proof);
  if (!admission.ok()) return admission;
  if (descriptor.descriptor_store_scan) {
    return Refuse(InMemoryIndexOpenClass::descriptor_scan_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.DESCRIPTOR_SCAN_REFUSED",
                  "index.in_memory.descriptor_scan_refused",
                  "descriptor_store_scan_refused");
  }
  if (descriptor.behavior_store_scan) {
    return Refuse(InMemoryIndexOpenClass::behavior_scan_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.BEHAVIOR_SCAN_REFUSED",
                  "index.in_memory.behavior_scan_refused",
                  "behavior_store_scan_refused");
  }
  if (DescriptorAuthorityClaimed(descriptor)) {
    return Refuse(InMemoryIndexOpenClass::authority_claim_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.AUTHORITY_CLAIM_REFUSED",
                  "index.in_memory.authority_claim_refused",
                  "in_memory_cold_source_cannot_own_authority");
  }
  if (DescriptorUnsafeFallbackClaimed(descriptor)) {
    return Refuse(InMemoryIndexOpenClass::unsafe_fallback_refused,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.UNSAFE_FALLBACK_REFUSED",
                  "index.in_memory.unsafe_fallback_refused",
                  "unsafe_in_memory_cold_source_fallback_mode");
  }
  if (!DescriptorSafe(*runtime, descriptor)) {
    return Refuse(InMemoryIndexOpenClass::corrupt_cold_source,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.CORRUPT_COLD_SOURCE",
                  "index.in_memory.corrupt_cold_source",
                  "in_memory_cold_source_invalid");
  }

  bool entries_valid = false;
  auto entries =
      CanonicalEntries(std::move(descriptor.entries), &entries_valid);
  if (!entries_valid) {
    return Refuse(InMemoryIndexOpenClass::corrupt_cold_source,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.CORRUPT_COLD_SOURCE",
                  "index.in_memory.corrupt_cold_source",
                  "in_memory_cold_source_entry_invalid");
  }

  std::lock_guard<std::mutex> guard(runtime->publish_mutex);
  auto result = PublishGeneration(
      runtime, std::move(entries), descriptor.descriptor_epoch,
      descriptor.persisted_generation, 1,
      {"in_memory.cold_rebuild=true",
       "in_memory.warm_reopen_from_cold_source=true",
       CountEvidence("in_memory.source_descriptor_epoch",
                     descriptor.descriptor_epoch),
       CountEvidence("in_memory.persisted_generation",
                     descriptor.persisted_generation)});
  if (result.ok()) {
    runtime->total_rebuilds += 1;
  }
  return result;
}

InMemoryIndexResult LookupInMemoryIndex(
    const InMemoryIndexRuntimeState* runtime,
    const InMemoryIndexLookupRequest& request) {
  auto admission =
      ValidateRuntimeAndProof(runtime, request.runtime_epoch, request.proof);
  if (!admission.ok()) return admission;
  auto generation = LoadGeneration(*runtime);
  if (!generation) {
    return Refuse(InMemoryIndexOpenClass::dropped, ErrorStatus(),
                  "INDEX.IN_MEMORY.GENERATION_MISSING",
                  "index.in_memory.generation_missing",
                  "in_memory_generation_missing");
  }
  if (generation->runtime_epoch != runtime->options.runtime_epoch) {
    return Refuse(InMemoryIndexOpenClass::stale_runtime_epoch,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
                  "index.in_memory.stale_runtime_epoch",
                  "in_memory_generation_epoch_stale");
  }

  InMemoryIndexResult result;
  result.status = OkStatus();
  result.open_class = InMemoryIndexOpenClass::current;
  result.generation = generation;
  result.evidence = BaseEvidence();
  result.evidence.push_back("in_memory.lookup.snapshot_safe=true");
  result.evidence.push_back("in_memory.lookup.immutable_snapshot_read=true");
  result.evidence.push_back("in_memory.lookup.mutable_shared_read=false");
  result.evidence.push_back("in_memory.lookup.candidates_only=true");
  result.evidence.push_back(std::string("in_memory.lookup.mode=") +
                            InMemoryIndexLookupModeName(request.mode));
  result.evidence.push_back(
      CountEvidence("in_memory.lookup.generation_id",
                    generation->generation_id));

  if (request.mode == InMemoryIndexLookupMode::point) {
    auto it = generation->entries_by_key.find(request.key);
    if (it != generation->entries_by_key.end()) {
      result.candidates = it->second;
    }
  } else if (request.mode == InMemoryIndexLookupMode::range) {
    if (request.lower_key.empty() || request.upper_key.empty() ||
        request.upper_key < request.lower_key) {
      return Refuse(InMemoryIndexOpenClass::refused, ErrorStatus(),
                    "INDEX.IN_MEMORY.INVALID_RANGE",
                    "index.in_memory.invalid_range",
                    "in_memory_invalid_range_lookup");
    }
    auto it = request.lower_inclusive
                  ? generation->entries_by_key.lower_bound(request.lower_key)
                  : generation->entries_by_key.upper_bound(request.lower_key);
    for (; it != generation->entries_by_key.end(); ++it) {
      const bool before_upper =
          request.upper_inclusive ? it->first <= request.upper_key
                                  : it->first < request.upper_key;
      if (!before_upper) break;
      result.candidates.insert(result.candidates.end(), it->second.begin(),
                               it->second.end());
    }
  } else {
    if (request.prefix.empty()) {
      return Refuse(InMemoryIndexOpenClass::refused, ErrorStatus(),
                    "INDEX.IN_MEMORY.INVALID_PREFIX",
                    "index.in_memory.invalid_prefix",
                    "in_memory_invalid_prefix_lookup");
    }
    for (auto it = generation->entries_by_key.lower_bound(request.prefix);
         it != generation->entries_by_key.end(); ++it) {
      if (it->first.compare(0, request.prefix.size(), request.prefix) != 0) {
        break;
      }
      result.candidates.insert(result.candidates.end(), it->second.begin(),
                               it->second.end());
    }
  }
  result.evidence.push_back(
      CountEvidence("in_memory.lookup.candidate_count",
                    static_cast<u64>(result.candidates.size())));
  return result;
}

InMemoryIndexResult ApplyInMemoryIndexMutation(
    InMemoryIndexRuntimeState* runtime,
    const InMemoryIndexMutation& mutation) {
  auto admission =
      ValidateRuntimeAndProof(runtime, mutation.runtime_epoch, mutation.proof);
  if (!admission.ok()) return admission;
  if (!EntryValid(mutation.entry)) {
    return Refuse(InMemoryIndexOpenClass::corrupt_cold_source,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.INVALID_MUTATION_ENTRY",
                  "index.in_memory.invalid_mutation_entry",
                  "in_memory_mutation_entry_invalid");
  }

  std::lock_guard<std::mutex> guard(runtime->publish_mutex);
  auto current = LoadGeneration(*runtime);
  if (!current) {
    return Refuse(InMemoryIndexOpenClass::dropped, ErrorStatus(),
                  "INDEX.IN_MEMORY.GENERATION_MISSING",
                  "index.in_memory.generation_missing",
                  "in_memory_generation_missing");
  }
  if (current->runtime_epoch != runtime->options.runtime_epoch) {
    return Refuse(InMemoryIndexOpenClass::stale_runtime_epoch,
                  ErrorStatus(),
                  "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
                  "index.in_memory.stale_runtime_epoch",
                  "in_memory_generation_epoch_stale");
  }

  auto next_entries = current->entries_by_key;
  auto& values = next_entries[mutation.entry.key];
  bool changed = false;
  if (mutation.kind == InMemoryIndexMutationKind::insert_entry) {
    const bool duplicate_identity =
        std::any_of(values.begin(), values.end(), [&](const auto& entry) {
          return SameEntryIdentity(entry, mutation.entry);
        });
    if (duplicate_identity) {
      return Refuse(InMemoryIndexOpenClass::refused, ErrorStatus(),
                    "INDEX.IN_MEMORY.DUPLICATE_ENTRY_REFUSED",
                    "index.in_memory.duplicate_entry_refused",
                    "in_memory_duplicate_row_locator_refused");
    }
    values.push_back(mutation.entry);
    changed = true;
  } else if (mutation.kind == InMemoryIndexMutationKind::update_entry) {
    if (mutation.replacement_exact_source_token.empty()) {
      return Refuse(InMemoryIndexOpenClass::corrupt_cold_source,
                    ErrorStatus(),
                    "INDEX.IN_MEMORY.INVALID_MUTATION_ENTRY",
                    "index.in_memory.invalid_mutation_entry",
                    "in_memory_update_exact_source_token_missing");
    }
    for (auto& entry : values) {
      if (SameEntryIdentity(entry, mutation.entry)) {
        entry.payload = mutation.replacement_payload;
        entry.exact_source_token = mutation.replacement_exact_source_token;
        changed = EntryValid(entry);
        break;
      }
    }
  } else {
    const auto old_size = values.size();
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&](const auto& entry) {
                                  return SameEntryIdentity(entry,
                                                           mutation.entry);
                                }),
                 values.end());
    changed = values.size() != old_size;
    if (values.empty()) {
      next_entries.erase(mutation.entry.key);
    }
  }
  if (!changed) {
    return Refuse(InMemoryIndexOpenClass::refused, ErrorStatus(),
                  "INDEX.IN_MEMORY.MUTATION_REFUSED",
                  "index.in_memory.mutation_refused",
                  "in_memory_mutation_no_matching_entry");
  }
  for (auto& [key, entries] : next_entries) {
    (void)key;
    std::sort(entries.begin(), entries.end(), EntryLess);
  }

  auto result = PublishGeneration(
      runtime, std::move(next_entries), current->source_descriptor_epoch,
      current->persisted_generation, current->generation_id + 1,
      {"in_memory.mutation.copy_on_write=true",
       "in_memory.mutation.published_new_generation=true",
       std::string("in_memory.mutation.kind=") +
           InMemoryIndexMutationKindName(mutation.kind)});
  if (result.ok()) {
    runtime->total_mutations += 1;
  }
  return result;
}

InMemoryIndexSupportBundle BuildInMemoryIndexSupportBundle(
    const InMemoryIndexRuntimeState* runtime) {
  if (runtime == nullptr) {
    return SupportRefuse(ErrorStatus(),
                         "INDEX.IN_MEMORY.RUNTIME_MISSING",
                         "index.in_memory.runtime_missing",
                         "runtime_state_missing");
  }
  InMemoryIndexSupportBundle result;
  result.status = OkStatus();
  result.dropped = runtime->dropped;
  result.runtime_epoch = runtime->options.runtime_epoch;
  result.current_memory_bytes = runtime->current_memory_bytes;
  result.peak_memory_bytes = runtime->peak_memory_bytes;
  result.total_denied_bytes = runtime->total_denied_bytes;
  result.evidence = BaseEvidence();
  result.evidence.push_back("in_memory.support_bundle=true");
  result.evidence.push_back(
      CountEvidence("in_memory.runtime_epoch", result.runtime_epoch));
  result.evidence.push_back(
      CountEvidence("in_memory.memory.current_bytes",
                    result.current_memory_bytes));
  result.evidence.push_back(
      CountEvidence("in_memory.memory.peak_bytes",
                    result.peak_memory_bytes));
  result.evidence.push_back(
      CountEvidence("in_memory.memory.total_denied_bytes",
                    result.total_denied_bytes));
  result.evidence.push_back(std::string("in_memory.dropped=") +
                            (result.dropped ? "true" : "false"));

  auto generation = LoadGeneration(*runtime);
  if (generation) {
    if (generation->runtime_epoch != runtime->options.runtime_epoch) {
      return SupportRefuse(ErrorStatus(),
                           "INDEX.IN_MEMORY.STALE_RUNTIME_EPOCH",
                           "index.in_memory.stale_runtime_epoch",
                           "in_memory_generation_epoch_stale");
    }
    result.generation_id = generation->generation_id;
    result.entry_count = EntryCount(generation->entries_by_key);
    result.evidence.push_back(
        CountEvidence("in_memory.generation_id", result.generation_id));
    result.evidence.push_back(
        CountEvidence("in_memory.entry_count", result.entry_count));
    result.evidence.push_back("in_memory.support_bundle.snapshot_loaded=true");
  }
  return result;
}

InMemoryIndexSupportBundle DropInMemoryIndexRuntime(
    InMemoryIndexRuntimeState* runtime) {
  if (runtime == nullptr) {
    return SupportRefuse(ErrorStatus(),
                         "INDEX.IN_MEMORY.RUNTIME_MISSING",
                         "index.in_memory.runtime_missing",
                         "runtime_state_missing");
  }
  std::lock_guard<std::mutex> guard(runtime->publish_mutex);
  const u64 released = runtime->current_memory_bytes;
  StoreGeneration(runtime, {});
  runtime->current_memory_bytes = 0;
  runtime->dropped = true;

  InMemoryIndexSupportBundle result;
  result.status = OkStatus();
  result.dropped = true;
  result.runtime_epoch = runtime->options.runtime_epoch;
  result.peak_memory_bytes = runtime->peak_memory_bytes;
  result.total_denied_bytes = runtime->total_denied_bytes;
  result.evidence = BaseEvidence();
  result.evidence.push_back("in_memory.drop.cleaned=true");
  result.evidence.push_back("in_memory.drop.generation_released=true");
  result.evidence.push_back(
      CountEvidence("in_memory.memory.released_bytes", released));
  result.evidence.push_back("in_memory.support_bundle=true");
  return result;
}

DiagnosticRecord MakeInMemoryIndexRuntimeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back(DiagnosticArgument{"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.in_memory_runtime";
  return record;
}

}  // namespace scratchbird::core::index
