// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ngram_physical_provider.hpp"

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

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'N', 'G', 'R', 'M', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

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

bool GramWidthValid(u32 width) {
  return width >= kNgramPhysicalProviderMinGramWidth &&
         width <= kNgramPhysicalProviderMaxGramWidth;
}

bool GramValid(const std::string& gram, u32 width) {
  return gram.size() == width &&
         std::all_of(gram.begin(), gram.end(), [](unsigned char ch) {
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

bool RecheckProofValid(const NgramExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_batch_available &&
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

bool TokenizerAuthorityClean(const NgramTokenizerDescriptor& tokenizer) {
  return !tokenizer.parser_finality_authority_claimed &&
         !tokenizer.reference_finality_authority_claimed &&
         !tokenizer.provider_finality_authority_claimed &&
         !tokenizer.index_finality_authority_claimed &&
         !tokenizer.write_ahead_log_finality_authority_claimed;
}

bool TokenizerSafe(const NgramTokenizerDescriptor& tokenizer) {
  return tokenizer.tokenizer_epoch > 0 &&
         tokenizer.charset_epoch > 0 &&
         tokenizer.resource_epoch > 0 &&
         tokenizer.deterministic &&
         tokenizer.tokenizer_safe &&
         tokenizer.charset_safe &&
         tokenizer.unicode_boundary_safe &&
         !tokenizer.descriptor_store_scan &&
         !tokenizer.behavior_store_scan &&
         TokenizerAuthorityClean(tokenizer);
}

bool ProviderAuthorityClean(const NgramPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         provider.exact_source_recheck_required &&
         provider.mga_recheck_required &&
         provider.security_recheck_required &&
         !provider.descriptor_store_scan &&
         !provider.behavior_store_scan &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

bool SourceRowValid(const NgramSourceRow& row) {
  return LocatorValid(row.locator) && !row.exact_source_value.empty();
}

bool PostingEntryValid(const NgramPostingEntry& entry, u32 width) {
  return GramValid(entry.gram, width) &&
         !entry.postings.empty() &&
         std::all_of(entry.postings.begin(), entry.postings.end(),
                     LocatorValid) &&
         std::is_sorted(entry.postings.begin(), entry.postings.end(),
                        LocatorLess) &&
         std::adjacent_find(entry.postings.begin(), entry.postings.end(),
                            LocatorEqual) == entry.postings.end();
}

bool ProviderValid(const NgramPhysicalProvider& provider) {
  if (provider.artifact_kind != kNgramPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kNgramPhysicalProviderCurrentMajor,
                          kNgramPhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      !TokenizerSafe(provider.tokenizer) ||
      !GramWidthValid(provider.gram_width) ||
      !provider.qgram_extractor_present ||
      !provider.qgram_postings_present ||
      !provider.prefix_acceleration_present ||
      !provider.suffix_acceleration_present ||
      !provider.contains_acceleration_present ||
      !provider.false_positive_tracking_present ||
      !provider.exact_source_recheck_batching_present ||
      !ProviderAuthorityClean(provider) ||
      provider.source_rows.empty() ||
      provider.postings.empty()) {
    return false;
  }
  u64 previous_row = 0;
  for (const auto& row : provider.source_rows) {
    if (!SourceRowValid(row) || row.locator.row_ordinal <= previous_row) {
      return false;
    }
    previous_row = row.locator.row_ordinal;
  }
  std::string previous_gram;
  bool first = true;
  for (const auto& entry : provider.postings) {
    if (!PostingEntryValid(entry, provider.gram_width) ||
        (!first && entry.gram <= previous_gram)) {
      return false;
    }
    previous_gram = entry.gram;
    first = false;
  }
  return true;
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

NgramPhysicalBuildResult BuildFailure(std::string code,
                                      std::string key,
                                      std::string detail = {}) {
  NgramPhysicalBuildResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeNgramPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

NgramPhysicalOpenResult OpenFailure(NgramPhysicalOpenClass open_class,
                                    std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  NgramPhysicalOpenResult result;
  result.status =
      open_class == NgramPhysicalOpenClass::stale_format ||
              open_class == NgramPhysicalOpenClass::stale_generation ||
              open_class == NgramPhysicalOpenClass::stale_epoch
          ? WarnStatus()
          : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == NgramPhysicalOpenClass::bad_checksum ||
      open_class == NgramPhysicalOpenClass::corrupt_payload;
  result.diagnostic = MakeNgramPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_ngram_physical_provider");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_rows_and_exact_recheck");
  }
  return result;
}

NgramQueryResult QueryFailure(std::string code,
                              std::string key,
                              std::string detail = {}) {
  NgramQueryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeNgramPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back("ngram_physical_provider.candidates_refused=true");
  result.evidence.push_back("ngram_physical_provider.exact_source_recheck_required=true");
  result.evidence.push_back("ngram_physical_provider.mga_recheck_required=true");
  result.evidence.push_back("ngram_physical_provider.security_recheck_required=true");
  return result;
}

NgramPhysicalMutationResult MutationFailure(NgramPhysicalProvider provider,
                                            std::string code,
                                            std::string key,
                                            std::string detail = {}) {
  NgramPhysicalMutationResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeNgramPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_ngram_maintenance_mutation");
  return result;
}

std::string NormalizeAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

void AddPosting(std::map<std::string, std::vector<TextInvertedRowLocator>>* map,
                const std::string& gram,
                const TextInvertedRowLocator& locator) {
  auto& postings = (*map)[gram];
  if (std::find_if(postings.begin(), postings.end(), [&](const auto& item) {
        return LocatorEqual(item, locator);
      }) == postings.end()) {
    postings.push_back(locator);
  }
}

std::map<std::string, std::vector<TextInvertedRowLocator>> BuildPostingMap(
    const std::vector<NgramSourceRow>& rows,
    u32 gram_width) {
  std::map<std::string, std::vector<TextInvertedRowLocator>> map;
  for (const auto& row : rows) {
    for (const auto& gram :
         ExtractNgramsForProvider(row.exact_source_value, gram_width)) {
      AddPosting(&map, gram, row.locator);
    }
  }
  return map;
}

std::vector<NgramPostingEntry> EntriesFromMap(
    std::map<std::string, std::vector<TextInvertedRowLocator>> map) {
  std::vector<NgramPostingEntry> entries;
  for (auto& [gram, postings] : map) {
    std::sort(postings.begin(), postings.end(), LocatorLess);
    postings.erase(std::unique(postings.begin(), postings.end(), LocatorEqual),
                   postings.end());
    if (postings.empty()) continue;
    entries.push_back({std::move(gram), std::move(postings)});
  }
  return entries;
}

void SetProviderEvidence(NgramPhysicalProvider* provider) {
  provider->evidence = {
      kNgramPhysicalProviderSearchKey,
      "ngram_physical_provider.qgram_extraction_present=true",
      "ngram_physical_provider.qgram_posting_storage_present=true",
      "ngram_physical_provider.prefix_acceleration_present=true",
      "ngram_physical_provider.suffix_acceleration_present=true",
      "ngram_physical_provider.contains_acceleration_present=true",
      "ngram_physical_provider.false_positive_tracking_present=true",
      "ngram_physical_provider.exact_source_recheck_batching_present=true",
      "ngram_physical_provider.candidate_evidence_only=true",
      "ngram_physical_provider.index_finality_authority=false",
      "ngram_physical_provider.write_ahead_log_authority=false"};
}

void NormalizeProvider(NgramPhysicalProvider* provider) {
  std::sort(provider->source_rows.begin(), provider->source_rows.end(),
            [](const auto& left, const auto& right) {
              return LocatorLess(left.locator, right.locator);
            });
  provider->postings =
      EntriesFromMap(BuildPostingMap(provider->source_rows,
                                     provider->gram_width));
  SetProviderEvidence(provider);
}

bool PostingEntriesEqual(const std::vector<NgramPostingEntry>& left,
                         const std::vector<NgramPostingEntry>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].gram != right[i].gram ||
        left[i].postings.size() != right[i].postings.size()) {
      return false;
    }
    for (std::size_t j = 0; j < left[i].postings.size(); ++j) {
      if (!LocatorEqual(left[i].postings[j], right[i].postings[j])) {
        return false;
      }
    }
  }
  return true;
}

bool PostingsMatchSourceRows(const NgramPhysicalProvider& provider) {
  return PostingEntriesEqual(
      provider.postings,
      EntriesFromMap(BuildPostingMap(provider.source_rows,
                                     provider.gram_width)));
}

bool BuildRequestValid(const NgramPhysicalBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.provider_uuid) &&
         request.base_generation > 0 &&
         request.provider_generation > 0 &&
         TokenizerSafe(request.tokenizer) &&
         GramWidthValid(request.gram_width) &&
         RecheckProofValid(request.recheck_proof) &&
         !request.rows.empty() &&
         std::all_of(request.rows.begin(), request.rows.end(), SourceRowValid);
}

const NgramPostingEntry* FindPosting(const NgramPhysicalProvider& provider,
                                     const std::string& gram) {
  auto iter = std::lower_bound(
      provider.postings.begin(), provider.postings.end(), gram,
      [](const NgramPostingEntry& entry, const std::string& value) {
        return entry.gram < value;
      });
  if (iter == provider.postings.end() || iter->gram != gram) {
    return nullptr;
  }
  return &*iter;
}

const NgramSourceRow* FindSourceRow(const NgramPhysicalProvider& provider,
                                    const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider.source_rows.begin(), provider.source_rows.end(), locator,
      [](const NgramSourceRow& row, const TextInvertedRowLocator& value) {
        return LocatorLess(row.locator, value);
      });
  if (iter == provider.source_rows.end() ||
      !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

bool RuntimeRequestSafe(const NgramQueryRequest& request) {
  return ProviderValid(request.provider) &&
         RecheckProofValid(request.recheck_proof) &&
         request.tokenizer_epoch_current &&
         request.charset_epoch_current &&
         request.resource_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.pattern.empty();
}

bool ExactMatch(NgramQueryKind kind,
                const std::string& source,
                const std::string& pattern) {
  const std::string normalized_source = NormalizeAscii(source);
  const std::string normalized_pattern = NormalizeAscii(pattern);
  switch (kind) {
    case NgramQueryKind::prefix:
      return normalized_source.rfind(normalized_pattern, 0) == 0;
    case NgramQueryKind::suffix:
      return normalized_source.size() >= normalized_pattern.size() &&
             normalized_source.compare(normalized_source.size() -
                                           normalized_pattern.size(),
                                       normalized_pattern.size(),
                                       normalized_pattern) == 0;
    case NgramQueryKind::contains:
      return normalized_source.find(normalized_pattern) != std::string::npos;
  }
  return false;
}

}  // namespace

std::vector<std::string> ExtractNgramsForProvider(std::string value,
                                                  u32 gram_width) {
  if (!GramWidthValid(gram_width)) {
    return {};
  }
  value = NormalizeAscii(std::move(value));
  std::vector<std::string> grams;
  if (value.size() < gram_width) {
    if (!value.empty()) {
      grams.push_back(value);
    }
  } else {
    for (std::size_t offset = 0; offset + gram_width <= value.size(); ++offset) {
      grams.push_back(value.substr(offset, gram_width));
    }
  }
  grams.erase(std::remove_if(grams.begin(), grams.end(),
                             [&](const auto& gram) {
                               return gram.size() != gram_width ||
                                      !GramValid(gram, gram_width);
                             }),
              grams.end());
  std::sort(grams.begin(), grams.end());
  grams.erase(std::unique(grams.begin(), grams.end()), grams.end());
  return grams;
}

NgramPhysicalBuildResult BuildNgramPhysicalProvider(
    const NgramPhysicalBuildRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.NGRAM_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.ngram_physical_provider.missing_exact_recheck",
        "ngram provider build requires an exact source recheck batch plus MGA/security proof");
  }
  if (!GramWidthValid(request.gram_width)) {
    return BuildFailure("INDEX.NGRAM_PHYSICAL_PROVIDER.INVALID_GRAM_WIDTH",
                        "index.ngram_physical_provider.invalid_gram_width");
  }
  if (!TokenizerSafe(request.tokenizer)) {
    return BuildFailure(
        "INDEX.NGRAM_PHYSICAL_PROVIDER.UNSAFE_TOKENIZER_OR_CHARSET",
        "index.ngram_physical_provider.unsafe_tokenizer_or_charset",
        "ngram provider build requires current deterministic safe tokenizer charset and resource epochs with no authority claims");
  }
  if (!BuildRequestValid(request)) {
    return BuildFailure(
        "INDEX.NGRAM_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.ngram_physical_provider.build_refused",
        "build requires valid identities epochs safe tokenizer charset resource state valid gram width exact recheck batch and clean authority flags");
  }
  std::vector<NgramSourceRow> rows = request.rows;
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (LocatorEqual(rows[i - 1].locator, rows[i].locator)) {
      return BuildFailure("INDEX.NGRAM_PHYSICAL_PROVIDER.DUPLICATE_ROW",
                          "index.ngram_physical_provider.duplicate_row");
    }
  }

  NgramPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.tokenizer = request.tokenizer;
  provider.gram_width = request.gram_width;
  provider.source_rows = std::move(rows);
  NormalizeProvider(&provider);
  if (!ProviderValid(provider)) {
    return BuildFailure("INDEX.NGRAM_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                        "index.ngram_physical_provider.build_corrupt");
  }

  NgramPhysicalBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

NgramPhysicalSerializeResult SerializeNgramPhysicalProvider(
    const NgramPhysicalProvider& provider) {
  NgramPhysicalSerializeResult result;
  if (!ProviderValid(provider) || !PostingsMatchSourceRows(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeNgramPhysicalProviderDiagnostic(
        result.status,
        "INDEX.NGRAM_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.ngram_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kNgramPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kNgramPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU64(&bytes, provider.tokenizer.tokenizer_epoch);
  AppendU64(&bytes, provider.tokenizer.charset_epoch);
  AppendU64(&bytes, provider.tokenizer.resource_epoch);
  AppendU32(&bytes, provider.gram_width);
  AppendU64(&bytes, static_cast<u64>(provider.source_rows.size()));
  for (const auto& row : provider.source_rows) {
    AppendLocator(&bytes, row.locator);
    AppendString(&bytes, row.exact_source_value);
  }
  AppendU64(&bytes, static_cast<u64>(provider.postings.size()));
  for (const auto& entry : provider.postings) {
    AppendString(&bytes, entry.gram);
    AppendU64(&bytes, static_cast<u64>(entry.postings.size()));
    for (const auto& locator : entry.postings) {
      AppendLocator(&bytes, locator);
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

NgramPhysicalOpenResult OpenNgramPhysicalProvider(
    const NgramPhysicalOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(NgramPhysicalOpenClass::missing_exact_recheck,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                       "index.ngram_physical_provider.missing_exact_recheck");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(NgramPhysicalOpenClass::bad_checksum,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.ngram_physical_provider.bad_checksum");
  }
  Reader reader(request.bytes);
  reader.SetOffset(8);
  NgramPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u64 row_count = 0;
  u64 posting_count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kNgramPhysicalProviderCurrentMajor,
                          kNgramPhysicalProviderCurrentMinor})) {
    return OpenFailure(NgramPhysicalOpenClass::stale_format,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.ngram_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU64(&provider.tokenizer.tokenizer_epoch) ||
      !reader.ReadU64(&provider.tokenizer.charset_epoch) ||
      !reader.ReadU64(&provider.tokenizer.resource_epoch) ||
      !reader.ReadU32(&provider.gram_width) ||
      !reader.ReadU64(&row_count) ||
      row_count > 1000000) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  provider.tokenizer.deterministic = true;
  provider.tokenizer.tokenizer_safe = true;
  provider.tokenizer.charset_safe = true;
  provider.tokenizer.unicode_boundary_safe = true;
  for (u64 i = 0; i < row_count; ++i) {
    NgramSourceRow row;
    if (!reader.ReadLocator(&row.locator) ||
        !reader.ReadString(&row.exact_source_value)) {
      return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                         "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.ngram_physical_provider.corrupt_payload");
    }
    provider.source_rows.push_back(std::move(row));
  }
  if (!reader.ReadU64(&posting_count) || posting_count > 1000000) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < posting_count; ++i) {
    NgramPostingEntry entry;
    u64 entry_posting_count = 0;
    if (!reader.ReadString(&entry.gram) ||
        !reader.ReadU64(&entry_posting_count) ||
        entry_posting_count > 1000000) {
      return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                         "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.ngram_physical_provider.corrupt_payload");
    }
    for (u64 j = 0; j < entry_posting_count; ++j) {
      TextInvertedRowLocator locator;
      if (!reader.ReadLocator(&locator)) {
        return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                           "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                           "index.ngram_physical_provider.corrupt_payload");
      }
      entry.postings.push_back(std::move(locator));
    }
    provider.postings.push_back(std::move(entry));
  }
  if (!reader.Done()) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(NgramPhysicalOpenClass::identity_mismatch,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.ngram_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(NgramPhysicalOpenClass::stale_generation,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.ngram_physical_provider.stale_generation");
  }
  if ((request.expected_tokenizer_epoch_present &&
       request.expected_tokenizer_epoch != provider.tokenizer.tokenizer_epoch) ||
      (request.expected_charset_epoch_present &&
       request.expected_charset_epoch != provider.tokenizer.charset_epoch) ||
      (request.expected_resource_epoch_present &&
       request.expected_resource_epoch != provider.tokenizer.resource_epoch)) {
    return OpenFailure(NgramPhysicalOpenClass::stale_epoch,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.STALE_EPOCH",
                       "index.ngram_physical_provider.stale_epoch");
  }
  if (!GramWidthValid(provider.gram_width)) {
    return OpenFailure(NgramPhysicalOpenClass::invalid_gram_width,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.INVALID_GRAM_WIDTH",
                       "index.ngram_physical_provider.invalid_gram_width");
  }
  if (!TokenizerSafe(provider.tokenizer)) {
    return OpenFailure(
        NgramPhysicalOpenClass::unsafe_tokenizer_or_charset,
        "INDEX.NGRAM_PHYSICAL_PROVIDER.UNSAFE_TOKENIZER_OR_CHARSET",
        "index.ngram_physical_provider.unsafe_tokenizer_or_charset");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }
  if (!PostingsMatchSourceRows(provider)) {
    return OpenFailure(NgramPhysicalOpenClass::corrupt_payload,
                       "INDEX.NGRAM_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.ngram_physical_provider.corrupt_payload");
  }

  NgramPhysicalOpenResult result;
  result.status = OkStatus();
  result.open_class = NgramPhysicalOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  return result;
}

NgramQueryResult QueryNgramPhysicalProvider(const NgramQueryRequest& request) {
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.NGRAM_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.ngram_physical_provider.runtime_refused",
        "runtime requires current tokenizer/charset/resource epochs exact source recheck batch and no descriptor/behavior scan");
  }
  const auto grams =
      ExtractNgramsForProvider(request.pattern, request.provider.gram_width);
  if (grams.empty()) {
    return QueryFailure("INDEX.NGRAM_PHYSICAL_PROVIDER.PATTERN_REFUSED",
                        "index.ngram_physical_provider.pattern_refused");
  }
  std::map<std::tuple<u64, std::string, std::string>, u32> hits;
  for (const auto& gram : grams) {
    const auto* entry = FindPosting(request.provider, gram);
    if (entry == nullptr) continue;
    for (const auto& locator : entry->postings) {
      hits[{locator.row_ordinal, locator.row_uuid, locator.version_uuid}] += 1;
    }
  }

  NgramQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.qgram_probe_count = grams.size();
  result.prefix_acceleration_used = request.kind == NgramQueryKind::prefix;
  result.suffix_acceleration_used = request.kind == NgramQueryKind::suffix;
  result.contains_acceleration_used = request.kind == NgramQueryKind::contains;
  result.evidence = request.provider.evidence;
  result.evidence.push_back("ngram_physical_provider.final_rows_authorized=false");
  result.evidence.push_back("ngram_physical_provider.descriptor_store_scan=false");
  result.evidence.push_back("ngram_physical_provider.behavior_store_scan=false");

  const u32 required = static_cast<u32>(grams.size());
  for (const auto& row : request.provider.source_rows) {
    const auto key = std::make_tuple(row.locator.row_ordinal,
                                     row.locator.row_uuid,
                                     row.locator.version_uuid);
    if (hits[key] != required) {
      continue;
    }
    NgramCandidate candidate;
    candidate.locator = row.locator;
    candidate.accelerated_candidate = true;
    candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
    candidate.exact_match =
        ExactMatch(request.kind, row.exact_source_value, request.pattern);
    candidate.false_positive = !candidate.exact_match;
    if (candidate.false_positive) {
      ++result.false_positive_count;
    }
    ++result.exact_recheck_batch_count;
    result.candidates.push_back(std::move(candidate));
  }
  result.candidates.erase(
      std::remove_if(result.candidates.begin(), result.candidates.end(),
                     [](const auto& candidate) {
                       return candidate.false_positive;
                     }),
      result.candidates.end());
  std::sort(result.candidates.begin(), result.candidates.end(),
            [](const auto& left, const auto& right) {
              return LocatorLess(left.locator, right.locator);
            });
  return result;
}

NgramPhysicalMutationResult ApplyNgramPhysicalMutation(
    const NgramPhysicalProvider& provider,
    const NgramPhysicalMutation& mutation) {
  if (!ProviderValid(provider) || !RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        provider,
        "INDEX.NGRAM_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.ngram_physical_provider.mutation_refused",
        "maintenance requires valid provider and exact source/MGA/security proof");
  }
  NgramPhysicalProvider next = provider;
  auto erase_row = [&](const NgramSourceRow& row) -> bool {
    if (!SourceRowValid(row)) return false;
    const auto before = next.source_rows.size();
    next.source_rows.erase(
        std::remove_if(next.source_rows.begin(), next.source_rows.end(),
                       [&](const auto& existing) {
                         return LocatorEqual(existing.locator, row.locator);
                       }),
        next.source_rows.end());
    return next.source_rows.size() != before;
  };
  auto insert_row = [&](const NgramSourceRow& row) -> bool {
    if (!SourceRowValid(row)) return false;
    if (FindSourceRow(next, row.locator) != nullptr) return false;
    next.source_rows.push_back(row);
    return true;
  };

  bool ok = true;
  switch (mutation.kind) {
    case NgramMutationKind::insert_row:
      ok = mutation.after_row_present && insert_row(mutation.after_row);
      break;
    case NgramMutationKind::delete_row:
      ok = mutation.before_row_present && erase_row(mutation.before_row);
      break;
    case NgramMutationKind::update_row:
      ok = mutation.before_row_present && mutation.after_row_present &&
           erase_row(mutation.before_row) && insert_row(mutation.after_row);
      break;
  }
  if (!ok) {
    return MutationFailure(
        next,
        "INDEX.NGRAM_PHYSICAL_PROVIDER.MUTATION_INPUT_REFUSED",
        "index.ngram_physical_provider.mutation_input_refused");
  }
  NormalizeProvider(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(next,
                           "INDEX.NGRAM_PHYSICAL_PROVIDER.MUTATION_CORRUPT",
                           "index.ngram_physical_provider.mutation_corrupt");
  }

  NgramPhysicalMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.actions.push_back("ngram_maintenance_candidate_provider_updated");
  return result;
}

const char* NgramPhysicalOpenClassName(NgramPhysicalOpenClass open_class) {
  switch (open_class) {
    case NgramPhysicalOpenClass::current: return "current";
    case NgramPhysicalOpenClass::stale_format: return "stale_format";
    case NgramPhysicalOpenClass::stale_generation: return "stale_generation";
    case NgramPhysicalOpenClass::bad_checksum: return "bad_checksum";
    case NgramPhysicalOpenClass::corrupt_payload: return "corrupt_payload";
    case NgramPhysicalOpenClass::identity_mismatch: return "identity_mismatch";
    case NgramPhysicalOpenClass::stale_epoch: return "stale_epoch";
    case NgramPhysicalOpenClass::unsafe_tokenizer_or_charset:
      return "unsafe_tokenizer_or_charset";
    case NgramPhysicalOpenClass::invalid_gram_width:
      return "invalid_gram_width";
    case NgramPhysicalOpenClass::missing_exact_recheck:
      return "missing_exact_recheck";
    case NgramPhysicalOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case NgramPhysicalOpenClass::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeNgramPhysicalProviderDiagnostic(
    Status status,
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
  record.source_component = "sb_core_index.ngram_physical_provider";
  record.remediation_hint =
      "use current safe tokenizer/charset resources and exact source/MGA/security recheck before row admission";
  return record;
}

}  // namespace scratchbird::core::index
