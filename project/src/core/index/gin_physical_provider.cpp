// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "gin_physical_provider.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'G', 'I', 'N', 'P', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;
inline constexpr u32 kMaxPostingListLimit = 4096;
inline constexpr u32 kPostingTreePageSize = 3;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() {
  return {StatusCode::ok, Severity::warning, Subsystem::engine};
}
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool KeyValid(const std::string& key) {
  if (key.empty() || key.size() > 512) {
    return false;
  }
  return std::all_of(key.begin(), key.end(), [](unsigned char ch) {
    return ch > 0x20 && ch != 0x7f;
  });
}

bool LocatorValid(const TextInvertedRowLocator& locator) {
  return locator.row_ordinal > 0 &&
         PageExtentSummaryUuidTextValid(locator.row_uuid) &&
         PageExtentSummaryUuidTextValid(locator.version_uuid);
}

bool LocatorLess(const TextInvertedRowLocator& left,
                 const TextInvertedRowLocator& right) {
  return std::tie(left.row_ordinal, left.row_uuid, left.version_uuid) <
         std::tie(right.row_ordinal, right.row_uuid, right.version_uuid);
}

bool LocatorEqual(const TextInvertedRowLocator& left,
                  const TextInvertedRowLocator& right) {
  return !LocatorLess(left, right) && !LocatorLess(right, left);
}

bool RecheckProofValid(const GinExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_recheck_required &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         !proof.parser_finality_authority_claimed &&
         !proof.reference_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool OpclassAuthorityClean(const GinOpclassDescriptor& opclass) {
  return !opclass.parser_finality_authority_claimed &&
         !opclass.reference_finality_authority_claimed &&
         !opclass.provider_finality_authority_claimed &&
         !opclass.index_finality_authority_claimed &&
         !opclass.write_ahead_log_finality_authority_claimed;
}

bool OpclassSafe(const GinOpclassDescriptor& opclass) {
  return !opclass.opclass_name.empty() &&
         opclass.opclass_epoch > 0 &&
         opclass.resource_epoch > 0 &&
         opclass.deterministic &&
         opclass.immutable &&
         opclass.safe &&
         opclass.tri_consistent_supported &&
         !opclass.descriptor_store_scan &&
         !opclass.behavior_store_scan &&
         !opclass.contract_only_fallback &&
         !opclass.provider_only_fallback &&
         OpclassAuthorityClean(opclass);
}

bool ProviderAuthorityClean(const GinPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         provider.exact_source_recheck_required &&
         provider.mga_recheck_required &&
         provider.security_recheck_required &&
         !provider.descriptor_store_scan &&
         !provider.behavior_store_scan &&
         !provider.contract_only_fallback &&
         !provider.provider_only_fallback &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

bool SourceRowValid(const GinSourceRow& row) {
  return LocatorValid(row.locator) && !row.exact_source_value.empty();
}

bool EntryValid(const GinPostingStorageEntry& entry, u32 posting_list_limit) {
  if (!KeyValid(entry.key) ||
      (entry.posting_list_used == entry.posting_tree_used) ||
      (entry.posting_list_used &&
       (entry.posting_list.empty() ||
        entry.posting_list.size() > posting_list_limit ||
        !entry.posting_tree_pages.empty())) ||
      (entry.posting_tree_used &&
       (entry.posting_tree_pages.empty() || !entry.posting_list.empty()))) {
    return false;
  }
  std::vector<TextInvertedRowLocator> all;
  if (entry.posting_list_used) {
    all = entry.posting_list;
  } else {
    for (const auto& page : entry.posting_tree_pages) {
      if (page.empty() || page.size() > kPostingTreePageSize) {
        return false;
      }
      all.insert(all.end(), page.begin(), page.end());
    }
  }
  return !all.empty() &&
         std::all_of(all.begin(), all.end(), LocatorValid) &&
         std::is_sorted(all.begin(), all.end(), LocatorLess) &&
         std::adjacent_find(all.begin(), all.end(), LocatorEqual) == all.end();
}

bool ProviderValid(const GinPhysicalProvider& provider) {
  if (provider.artifact_kind != kGinPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kGinPhysicalProviderCurrentMajor,
                          kGinPhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      !OpclassSafe(provider.opclass) ||
      provider.pending_flush_threshold == 0 ||
      provider.posting_list_limit == 0 ||
      provider.posting_list_limit > kMaxPostingListLimit ||
      !provider.pending_list_present ||
      !provider.posting_lists_present ||
      !provider.tri_consistent_executor_present ||
      !ProviderAuthorityClean(provider)) {
    return false;
  }
  std::string previous_key;
  bool first = true;
  bool saw_tree = false;
  for (const auto& pending : provider.pending_list) {
    if (!KeyValid(pending.key) || !LocatorValid(pending.locator)) {
      return false;
    }
  }
  for (const auto& entry : provider.entries) {
    if (!EntryValid(entry, provider.posting_list_limit) ||
        (!first && entry.key <= previous_key)) {
      return false;
    }
    saw_tree = saw_tree || entry.posting_tree_used;
    previous_key = entry.key;
    first = false;
  }
  return provider.posting_trees_present == saw_tree;
}

void AppendU8(std::vector<byte>* out, byte value) { out->push_back(value); }

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendBool(std::vector<byte>* out, bool value) {
  AppendU8(out, static_cast<byte>(value ? 1 : 0));
}

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLocator(std::vector<byte>* out,
                   const TextInvertedRowLocator& locator) {
  AppendU64(out, locator.row_ordinal);
  AppendString(out, locator.row_uuid);
  AppendString(out, locator.version_uuid);
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    StoreLittle64(bytes.data() + 16, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}
  void SetOffset(std::size_t offset) { offset_ = offset; }
  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) return false;
    *out = bytes_[offset_++];
    return true;
  }
  bool ReadBool(bool* out) {
    byte value = 0;
    if (!ReadU8(&value) || value > 1) return false;
    *out = value != 0;
    return true;
  }
  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) return false;
    *out = LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }
  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) return false;
    *out = LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }
  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) return false;
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  bool ReadLocator(TextInvertedRowLocator* out) {
    return ReadU64(&out->row_ordinal) &&
           ReadString(&out->row_uuid) &&
           ReadString(&out->version_uuid);
  }
  bool Done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

GinPhysicalBuildResult BuildFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  GinPhysicalBuildResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeGinPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GinPhysicalOpenResult OpenFailure(GinPhysicalOpenClass open_class,
                                  std::string code,
                                  std::string key,
                                  std::string detail = {}) {
  GinPhysicalOpenResult result;
  result.status = open_class == GinPhysicalOpenClass::stale_format ||
                          open_class == GinPhysicalOpenClass::stale_generation ||
                          open_class == GinPhysicalOpenClass::stale_epoch
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == GinPhysicalOpenClass::bad_checksum ||
      open_class == GinPhysicalOpenClass::corrupt_payload;
  result.diagnostic = MakeGinPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_gin_physical_provider");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_rows_and_exact_recheck");
  }
  return result;
}

GinTriConsistentResult QueryFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  GinTriConsistentResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeGinPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back("gin_physical_provider.candidates_refused=true");
  result.evidence.push_back("gin_physical_provider.exact_source_recheck_required=true");
  result.evidence.push_back("gin_physical_provider.mga_recheck_required=true");
  result.evidence.push_back("gin_physical_provider.security_recheck_required=true");
  return result;
}

GinPhysicalMutationResult MutationFailure(GinPhysicalProvider provider,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  GinPhysicalMutationResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeGinPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_gin_maintenance_mutation");
  return result;
}

std::vector<std::string> ExtractKeys(const GinSourceRow& row,
                                     const GinOpclassExtractor& extractor) {
  const auto extracted = extractor({row.exact_source_value, row.locator});
  if (!extracted.deterministic ||
      !extracted.exact_source_recheck_evidence_present ||
      extracted.evidence_ref.empty()) {
    return {};
  }
  std::vector<std::string> keys = extracted.keys;
  keys.erase(std::remove_if(keys.begin(), keys.end(),
                            [](const auto& key) { return !KeyValid(key); }),
             keys.end());
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

void AddPosting(std::map<std::string, std::vector<TextInvertedRowLocator>>* map,
                const std::string& key,
                const TextInvertedRowLocator& locator) {
  auto& postings = (*map)[key];
  if (std::find_if(postings.begin(), postings.end(), [&](const auto& item) {
        return LocatorEqual(item, locator);
      }) == postings.end()) {
    postings.push_back(locator);
  }
}

void RemovePosting(
    std::map<std::string, std::vector<TextInvertedRowLocator>>* map,
    const std::string& key,
    const TextInvertedRowLocator& locator) {
  const auto iter = map->find(key);
  if (iter == map->end()) return;
  auto& postings = iter->second;
  postings.erase(std::remove_if(postings.begin(), postings.end(),
                                [&](const auto& item) {
                                  return LocatorEqual(item, locator);
                                }),
                 postings.end());
  if (postings.empty()) {
    map->erase(iter);
  }
}

std::map<std::string, std::vector<TextInvertedRowLocator>> FlattenPostings(
    const GinPhysicalProvider& provider) {
  std::map<std::string, std::vector<TextInvertedRowLocator>> map;
  for (const auto& entry : provider.entries) {
    auto& postings = map[entry.key];
    if (entry.posting_list_used) {
      postings = entry.posting_list;
    } else {
      for (const auto& page : entry.posting_tree_pages) {
        postings.insert(postings.end(), page.begin(), page.end());
      }
    }
  }
  for (auto& [key, postings] : map) {
    std::sort(postings.begin(), postings.end(), LocatorLess);
    postings.erase(std::unique(postings.begin(), postings.end(), LocatorEqual),
                   postings.end());
  }
  return map;
}

std::vector<GinPostingStorageEntry> EntriesFromMap(
    std::map<std::string, std::vector<TextInvertedRowLocator>> map,
    u32 posting_list_limit) {
  std::vector<GinPostingStorageEntry> entries;
  for (auto& [key, postings] : map) {
    std::sort(postings.begin(), postings.end(), LocatorLess);
    postings.erase(std::unique(postings.begin(), postings.end(), LocatorEqual),
                   postings.end());
    if (postings.empty()) {
      continue;
    }
    GinPostingStorageEntry entry;
    entry.key = std::move(key);
    if (postings.size() <= posting_list_limit) {
      entry.posting_list_used = true;
      entry.posting_tree_used = false;
      entry.posting_list = std::move(postings);
    } else {
      entry.posting_list_used = false;
      entry.posting_tree_used = true;
      for (std::size_t offset = 0; offset < postings.size();
           offset += kPostingTreePageSize) {
        const auto end = std::min(postings.size(),
                                  offset + static_cast<std::size_t>(
                                               kPostingTreePageSize));
        entry.posting_tree_pages.emplace_back(postings.begin() + offset,
                                              postings.begin() + end);
      }
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

void FlushPending(GinPhysicalProvider* provider) {
  auto map = FlattenPostings(*provider);
  for (const auto& pending : provider->pending_list) {
    AddPosting(&map, pending.key, pending.locator);
  }
  provider->pending_list.clear();
  provider->entries = EntriesFromMap(std::move(map), provider->posting_list_limit);
  provider->posting_trees_present =
      std::any_of(provider->entries.begin(), provider->entries.end(),
                  [](const auto& entry) { return entry.posting_tree_used; });
}

void NormalizeEvidence(GinPhysicalProvider* provider) {
  provider->evidence = {
      kGinPhysicalProviderSearchKey,
      "gin_physical_provider.pending_list_present=true",
      "gin_physical_provider.posting_list_storage_present=true",
      provider->posting_trees_present
          ? "gin_physical_provider.posting_tree_storage_present=true"
          : "gin_physical_provider.posting_tree_storage_present=false",
      "gin_physical_provider.tri_consistent_executor_present=true",
      "gin_physical_provider.candidate_evidence_only=true",
      "gin_physical_provider.exact_source_mga_security_recheck_required=true",
      "gin_physical_provider.index_finality_authority=false",
      "gin_physical_provider.write_ahead_log_authority=false"};
}

bool BuildRequestValid(const GinPhysicalBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.provider_uuid) &&
         request.base_generation > 0 &&
         request.provider_generation > 0 &&
         request.pending_flush_threshold > 0 &&
         request.posting_list_limit > 0 &&
         request.posting_list_limit <= kMaxPostingListLimit &&
         OpclassSafe(request.opclass) &&
         RecheckProofValid(request.recheck_proof) &&
         static_cast<bool>(request.extractor) &&
         !request.rows.empty() &&
         std::all_of(request.rows.begin(), request.rows.end(), SourceRowValid);
}

std::vector<TextInvertedRowLocator> PostingsForEntry(
    const GinPostingStorageEntry& entry) {
  std::vector<TextInvertedRowLocator> postings = entry.posting_list;
  for (const auto& page : entry.posting_tree_pages) {
    postings.insert(postings.end(), page.begin(), page.end());
  }
  return postings;
}

const GinPostingStorageEntry* FindEntry(const GinPhysicalProvider& provider,
                                        const std::string& key) {
  auto iter = std::lower_bound(
      provider.entries.begin(), provider.entries.end(), key,
      [](const GinPostingStorageEntry& entry, const std::string& value) {
        return entry.key < value;
      });
  if (iter == provider.entries.end() || iter->key != key) {
    return nullptr;
  }
  return &*iter;
}

bool RuntimeRequestSafe(const GinTriConsistentRequest& request) {
  return request.opclass_epoch_current &&
         request.resource_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback &&
         RecheckProofValid(request.recheck_proof) &&
         ProviderValid(request.provider) &&
         !request.query_keys.empty() &&
         std::all_of(request.query_keys.begin(), request.query_keys.end(),
                     KeyValid);
}

}  // namespace

GinPhysicalBuildResult BuildGinPhysicalProvider(
    const GinPhysicalBuildRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.GIN_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.gin_physical_provider.missing_recheck_proof",
        "GIN provider build requires exact source/MGA/security recheck proof");
  }
  if (!OpclassSafe(request.opclass)) {
    return BuildFailure(
        "INDEX.GIN_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
        "index.gin_physical_provider.unsafe_opclass",
        "GIN provider build requires deterministic immutable safe opclass epochs and no descriptor/behavior scan fallback or authority claims");
  }
  if (!BuildRequestValid(request)) {
    return BuildFailure(
        "INDEX.GIN_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.gin_physical_provider.build_refused",
        "build requires valid identities epochs deterministic safe opclass extractor exact source/MGA/security proof and clean authority flags");
  }

  std::map<std::string, std::vector<TextInvertedRowLocator>> map;
  std::vector<GinPendingEntry> pending;
  std::set<std::tuple<u64, std::string, std::string>> seen_rows;
  for (const auto& row : request.rows) {
    if (!seen_rows.insert({row.locator.row_ordinal,
                           row.locator.row_uuid,
                           row.locator.version_uuid})
             .second) {
      return BuildFailure("INDEX.GIN_PHYSICAL_PROVIDER.DUPLICATE_ROW",
                          "index.gin_physical_provider.duplicate_row");
    }
    const auto keys = ExtractKeys(row, request.extractor);
    if (keys.empty()) {
      return BuildFailure(
          "INDEX.GIN_PHYSICAL_PROVIDER.EXTRACTOR_REFUSED",
          "index.gin_physical_provider.extractor_refused",
          "opclass extractor must produce deterministic keys with source recheck evidence");
    }
    for (const auto& key : keys) {
      if (pending.size() < request.pending_flush_threshold) {
        pending.push_back({key, row.locator});
      } else {
        AddPosting(&map, key, row.locator);
      }
    }
  }

  GinPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.opclass = request.opclass;
  provider.pending_flush_threshold = request.pending_flush_threshold;
  provider.posting_list_limit = request.posting_list_limit;
  provider.pending_list = std::move(pending);
  provider.entries = EntriesFromMap(std::move(map), provider.posting_list_limit);
  provider.posting_trees_present =
      std::any_of(provider.entries.begin(), provider.entries.end(),
                  [](const auto& entry) { return entry.posting_tree_used; });
  NormalizeEvidence(&provider);
  if (!ProviderValid(provider)) {
    return BuildFailure("INDEX.GIN_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                        "index.gin_physical_provider.build_corrupt");
  }

  GinPhysicalBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

GinPhysicalSerializeResult SerializeGinPhysicalProvider(
    const GinPhysicalProvider& provider) {
  GinPhysicalSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeGinPhysicalProviderDiagnostic(
        result.status,
        "INDEX.GIN_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.gin_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kGinPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kGinPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendString(&bytes, provider.opclass.opclass_name);
  AppendU64(&bytes, provider.opclass.opclass_epoch);
  AppendU64(&bytes, provider.opclass.resource_epoch);
  AppendU32(&bytes, provider.pending_flush_threshold);
  AppendU32(&bytes, provider.posting_list_limit);
  AppendU64(&bytes, static_cast<u64>(provider.pending_list.size()));
  for (const auto& pending : provider.pending_list) {
    AppendString(&bytes, pending.key);
    AppendLocator(&bytes, pending.locator);
  }
  AppendU64(&bytes, static_cast<u64>(provider.entries.size()));
  for (const auto& entry : provider.entries) {
    AppendString(&bytes, entry.key);
    AppendBool(&bytes, entry.posting_list_used);
    AppendBool(&bytes, entry.posting_tree_used);
    AppendU64(&bytes, static_cast<u64>(entry.posting_list.size()));
    for (const auto& locator : entry.posting_list) {
      AppendLocator(&bytes, locator);
    }
    AppendU64(&bytes, static_cast<u64>(entry.posting_tree_pages.size()));
    for (const auto& page : entry.posting_tree_pages) {
      AppendU64(&bytes, static_cast<u64>(page.size()));
      for (const auto& locator : page) {
        AppendLocator(&bytes, locator);
      }
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

GinPhysicalOpenResult OpenGinPhysicalProvider(
    const GinPhysicalOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(GinPhysicalOpenClass::missing_recheck_proof,
                       "INDEX.GIN_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
                       "index.gin_physical_provider.missing_recheck_proof");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 || stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(GinPhysicalOpenClass::bad_checksum,
                       "INDEX.GIN_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.gin_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  GinPhysicalProvider provider;
  if (!reader.ReadU32(&major) || !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kGinPhysicalProviderCurrentMajor,
                          kGinPhysicalProviderCurrentMinor})) {
    return OpenFailure(GinPhysicalOpenClass::stale_format,
                       "INDEX.GIN_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.gin_physical_provider.stale_format");
  }
  u64 pending_count = 0;
  u64 entry_count = 0;
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadString(&provider.opclass.opclass_name) ||
      !reader.ReadU64(&provider.opclass.opclass_epoch) ||
      !reader.ReadU64(&provider.opclass.resource_epoch) ||
      !reader.ReadU32(&provider.pending_flush_threshold) ||
      !reader.ReadU32(&provider.posting_list_limit) ||
      !reader.ReadU64(&pending_count) ||
      pending_count > 1000000) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }
  provider.opclass.deterministic = true;
  provider.opclass.immutable = true;
  provider.opclass.safe = true;
  provider.opclass.tri_consistent_supported = true;
  for (u64 i = 0; i < pending_count; ++i) {
    GinPendingEntry pending;
    if (!reader.ReadString(&pending.key) ||
        !reader.ReadLocator(&pending.locator)) {
      return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                         "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.gin_physical_provider.corrupt_payload");
    }
    provider.pending_list.push_back(std::move(pending));
  }
  if (!reader.ReadU64(&entry_count) || entry_count > 1000000) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < entry_count; ++i) {
    GinPostingStorageEntry entry;
    u64 list_count = 0;
    u64 page_count = 0;
    if (!reader.ReadString(&entry.key) ||
        !reader.ReadBool(&entry.posting_list_used) ||
        !reader.ReadBool(&entry.posting_tree_used) ||
        !reader.ReadU64(&list_count) ||
        list_count > 1000000) {
      return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                         "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.gin_physical_provider.corrupt_payload");
    }
    for (u64 j = 0; j < list_count; ++j) {
      TextInvertedRowLocator locator;
      if (!reader.ReadLocator(&locator)) {
        return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                           "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                           "index.gin_physical_provider.corrupt_payload");
      }
      entry.posting_list.push_back(std::move(locator));
    }
    if (!reader.ReadU64(&page_count) || page_count > 1000000) {
      return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                         "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.gin_physical_provider.corrupt_payload");
    }
    for (u64 page_index = 0; page_index < page_count; ++page_index) {
      u64 page_size = 0;
      if (!reader.ReadU64(&page_size) || page_size > kPostingTreePageSize) {
        return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                           "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                           "index.gin_physical_provider.corrupt_payload");
      }
      std::vector<TextInvertedRowLocator> page;
      for (u64 j = 0; j < page_size; ++j) {
        TextInvertedRowLocator locator;
        if (!reader.ReadLocator(&locator)) {
          return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                             "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                             "index.gin_physical_provider.corrupt_payload");
        }
        page.push_back(std::move(locator));
      }
      entry.posting_tree_pages.push_back(std::move(page));
    }
    provider.entries.push_back(std::move(entry));
  }
  NormalizeEvidence(&provider);
  if (!reader.Done()) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }
  provider.posting_trees_present =
      std::any_of(provider.entries.begin(), provider.entries.end(),
                  [](const auto& entry) { return entry.posting_tree_used; });

  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(GinPhysicalOpenClass::identity_mismatch,
                       "INDEX.GIN_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.gin_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(GinPhysicalOpenClass::stale_generation,
                       "INDEX.GIN_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.gin_physical_provider.stale_generation");
  }
  if ((request.expected_opclass_epoch_present &&
       request.expected_opclass_epoch != provider.opclass.opclass_epoch) ||
      (request.expected_resource_epoch_present &&
       request.expected_resource_epoch != provider.opclass.resource_epoch)) {
    return OpenFailure(GinPhysicalOpenClass::stale_epoch,
                       "INDEX.GIN_PHYSICAL_PROVIDER.STALE_EPOCH",
                       "index.gin_physical_provider.stale_epoch");
  }
  if (!OpclassSafe(provider.opclass)) {
    return OpenFailure(GinPhysicalOpenClass::unsafe_opclass,
                       "INDEX.GIN_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                       "index.gin_physical_provider.unsafe_opclass");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(GinPhysicalOpenClass::corrupt_payload,
                       "INDEX.GIN_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gin_physical_provider.corrupt_payload");
  }

  GinPhysicalOpenResult result;
  result.status = OkStatus();
  result.open_class = GinPhysicalOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  return result;
}

GinTriConsistentResult ExecuteGinTriConsistent(
    const GinTriConsistentRequest& request) {
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.GIN_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.gin_physical_provider.runtime_refused",
        "runtime requires current epochs exact recheck proof real provider storage and no descriptor/behavior scan or fallback");
  }
  std::map<std::tuple<u64, std::string, std::string>, GinCandidate> candidates;
  for (const auto& key : request.query_keys) {
    const auto* entry = FindEntry(request.provider, key);
    if (entry != nullptr) {
      const auto postings = PostingsForEntry(*entry);
      for (const auto& locator : postings) {
        auto& candidate = candidates[{locator.row_ordinal,
                                      locator.row_uuid,
                                      locator.version_uuid}];
        candidate.locator = locator;
        candidate.matched_key_count += 1;
        candidate.tri_state = GinTriState::yes;
        candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
      }
    }
  }
  u64 pending_matches = 0;
  for (const auto& pending : request.provider.pending_list) {
    if (std::find(request.query_keys.begin(), request.query_keys.end(),
                  pending.key) == request.query_keys.end()) {
      continue;
    }
    auto& candidate = candidates[{pending.locator.row_ordinal,
                                  pending.locator.row_uuid,
                                  pending.locator.version_uuid}];
    candidate.locator = pending.locator;
    candidate.matched_key_count += 1;
    candidate.from_pending_list = true;
    candidate.tri_state = GinTriState::maybe;
    candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
    ++pending_matches;
  }

  GinTriConsistentResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.tri_consistent_executor_used = true;
  result.posting_list_probe_count = 0;
  result.posting_tree_probe_count = 0;
  for (const auto& key : request.query_keys) {
    const auto* entry = FindEntry(request.provider, key);
    if (entry == nullptr) continue;
    if (entry->posting_list_used) ++result.posting_list_probe_count;
    if (entry->posting_tree_used) ++result.posting_tree_probe_count;
  }
  result.pending_list_probe_count = pending_matches;
  const u32 required =
      request.strategy == GinQueryStrategy::contains_all
          ? static_cast<u32>(request.query_keys.size())
          : 1;
  for (auto& [unused, candidate] : candidates) {
    (void)unused;
    if (candidate.matched_key_count >= required) {
      result.candidates.push_back(std::move(candidate));
    }
  }
  std::sort(result.candidates.begin(), result.candidates.end(),
            [](const auto& left, const auto& right) {
              return LocatorLess(left.locator, right.locator);
            });
  result.evidence = request.provider.evidence;
  result.evidence.push_back("gin_physical_provider.tri_consistent_executor_used=true");
  result.evidence.push_back("gin_physical_provider.final_rows_authorized=false");
  result.evidence.push_back("gin_physical_provider.descriptor_store_scan=false");
  result.evidence.push_back("gin_physical_provider.behavior_store_scan=false");
  return result;
}

GinPhysicalMutationResult ApplyGinPhysicalMutation(
    const GinPhysicalProvider& provider,
    const GinPhysicalMutation& mutation) {
  if (!ProviderValid(provider) || !RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        provider,
        "INDEX.GIN_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.gin_physical_provider.mutation_refused",
        "maintenance requires valid provider and exact source/MGA/security proof");
  }
  GinPhysicalProvider next = provider;
  auto map = FlattenPostings(next);
  auto remove_row = [&](const GinSourceRow& row) -> bool {
    if (!SourceRowValid(row) || !mutation.extractor) return false;
    const auto keys = ExtractKeys(row, mutation.extractor);
    if (keys.empty()) return false;
    next.pending_list.erase(
        std::remove_if(next.pending_list.begin(), next.pending_list.end(),
                       [&](const auto& pending) {
                         return LocatorEqual(pending.locator, row.locator);
                       }),
        next.pending_list.end());
    for (const auto& key : keys) {
      RemovePosting(&map, key, row.locator);
    }
    return true;
  };
  auto add_row = [&](const GinSourceRow& row) -> bool {
    if (!SourceRowValid(row) || !mutation.extractor) return false;
    const auto keys = ExtractKeys(row, mutation.extractor);
    if (keys.empty()) return false;
    for (const auto& key : keys) {
      next.pending_list.push_back({key, row.locator});
    }
    return true;
  };

  bool ok = true;
  switch (mutation.kind) {
    case GinMutationKind::insert_row:
      ok = mutation.after_row_present && add_row(mutation.after_row);
      break;
    case GinMutationKind::delete_row:
      ok = mutation.before_row_present && remove_row(mutation.before_row);
      break;
    case GinMutationKind::update_row:
      ok = mutation.before_row_present && mutation.after_row_present &&
           remove_row(mutation.before_row) && add_row(mutation.after_row);
      break;
    case GinMutationKind::flush_pending:
      break;
  }
  if (!ok) {
    return MutationFailure(next,
                           "INDEX.GIN_PHYSICAL_PROVIDER.MUTATION_INPUT_REFUSED",
                           "index.gin_physical_provider.mutation_input_refused");
  }
  next.entries = EntriesFromMap(std::move(map), next.posting_list_limit);
  bool flushed = mutation.kind == GinMutationKind::flush_pending ||
                 next.pending_list.size() >= next.pending_flush_threshold;
  if (flushed) {
    FlushPending(&next);
  } else {
    next.posting_trees_present =
        std::any_of(next.entries.begin(), next.entries.end(),
                    [](const auto& entry) { return entry.posting_tree_used; });
  }
  NormalizeEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(next,
                           "INDEX.GIN_PHYSICAL_PROVIDER.MUTATION_CORRUPT",
                           "index.gin_physical_provider.mutation_corrupt");
  }
  GinPhysicalMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.pending_flushed = flushed;
  result.fail_closed = false;
  result.actions.push_back("gin_maintenance_candidate_provider_updated");
  if (flushed) {
    result.actions.push_back("gin_pending_list_flushed_to_postings");
  }
  return result;
}

const char* GinPhysicalOpenClassName(GinPhysicalOpenClass open_class) {
  switch (open_class) {
    case GinPhysicalOpenClass::current: return "current";
    case GinPhysicalOpenClass::stale_format: return "stale_format";
    case GinPhysicalOpenClass::stale_generation: return "stale_generation";
    case GinPhysicalOpenClass::bad_checksum: return "bad_checksum";
    case GinPhysicalOpenClass::corrupt_payload: return "corrupt_payload";
    case GinPhysicalOpenClass::identity_mismatch: return "identity_mismatch";
    case GinPhysicalOpenClass::stale_epoch: return "stale_epoch";
    case GinPhysicalOpenClass::unsafe_opclass: return "unsafe_opclass";
    case GinPhysicalOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case GinPhysicalOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case GinPhysicalOpenClass::refused: return "refused";
  }
  return "refused";
}

const char* GinTriStateName(GinTriState state) {
  switch (state) {
    case GinTriState::no: return "no";
    case GinTriState::maybe: return "maybe";
    case GinTriState::yes: return "yes";
  }
  return "no";
}

DiagnosticRecord MakeGinPhysicalProviderDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.gin_physical_provider";
  record.remediation_hint =
      "use current deterministic opclass resources and perform exact source/MGA/security recheck before row admission";
  return record;
}

}  // namespace scratchbird::core::index
