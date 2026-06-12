// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/document_path_physical_provider.hpp"

#include "crud_support/crud_store.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kMagic = "SBDOCPATH";
constexpr std::uint64_t kFormatVersion = 1;
using PairList = std::vector<std::pair<std::string, std::string>>;

std::uint64_t Fnva64(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableProviderUuid(scratchbird::core::platform::UuidKind kind,
                               std::string seed) {
  if (seed.empty()) {
    seed = "scratchbird.document_path_provider.transient";
  }
  const auto left = Hex64(Fnva64(seed + ":left"));
  auto right = Hex64(Fnva64(seed + ":right"));
  std::string hex = left + right;
  hex[12] = '7';
  hex[16] = '8';
  auto parsed = scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
      kind,
      hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" +
          hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
          hex.substr(20, 12));
  if (parsed.ok()) {
    return scratchbird::core::uuid::UuidToString(parsed.value.value);
  }
  return GenerateCrudEngineUuid(kind == scratchbird::core::platform::UuidKind::database
                                    ? "database"
                                    : "object");
}

std::uint64_t ParseU64(const std::string& value) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    return consumed == value.size() ? parsed : 0;
  } catch (...) {
    return 0;
  }
}

std::int64_t ParseI64(const std::string& value, std::int64_t fallback = -1) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed);
    return consumed == value.size() ? parsed : fallback;
  } catch (...) {
    return fallback;
  }
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

bool ParseBool(const std::string& value) {
  return value == "true" || value == "1" || value == "TRUE";
}

std::string ValueOr(const std::map<std::string, std::string>& values,
                    const std::string& key,
                    const std::string& fallback = {}) {
  const auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

std::map<std::string, std::string> PairMap(const PairList& pairs) {
  std::map<std::string, std::string> values;
  for (const auto& [key, value] : pairs) {
    values[key] = value;
  }
  return values;
}

bool IsHex(char ch) {
  return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

bool IsValidUuid(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') { return false; }
    } else if (!IsHex(value[i])) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream in(value);
  while (std::getline(in, part, delimiter)) {
    parts.push_back(part);
  }
  return parts;
}

bool IsUnsignedIntegerToken(const std::string& token) {
  return !token.empty() &&
         std::all_of(token.begin(), token.end(), [](unsigned char ch) {
           return std::isdigit(ch) != 0;
         });
}

bool IsSafeToken(const std::string& token) {
  if (token.empty()) { return false; }
  if (token == "*") { return true; }
  return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
  });
}

bool IsSafePath(const std::string& path) {
  if (path.empty() || path.front() == '.' || path.back() == '.') {
    return false;
  }
  const auto parts = Split(path, '.');
  if (parts.empty()) { return false; }
  return std::all_of(parts.begin(), parts.end(), IsSafeToken);
}

std::string NormalizePath(const std::string& path) {
  std::string out;
  for (char ch : path) {
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string WildcardNormalizePath(const std::string& path) {
  const auto parts = Split(NormalizePath(path), '.');
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) { out.push_back('.'); }
    out += IsUnsignedIntegerToken(part) ? "*" : part;
  }
  return out;
}

std::int64_t FirstArrayPosition(const std::string& path) {
  for (const auto& part : Split(path, '.')) {
    if (IsUnsignedIntegerToken(part)) {
      return ParseI64(part);
    }
  }
  return -1;
}

std::string PostingKey(std::uint64_t path_id,
                       const std::string& scalar_type,
                       const std::string& encoded_value) {
  return std::to_string(path_id) + "\x1f" + scalar_type + "\x1f" + encoded_value;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ProviderDiagnostic(const char* detail) {
  return MakeInvalidRequestDiagnostic("nosql.document_path_physical_provider",
                                      detail);
}

DocumentPathProviderResult Failure(const char* detail) {
  DocumentPathProviderResult result;
  result.ok = false;
  result.diagnostic = ProviderDiagnostic(detail);
  result.evidence.push_back(std::string("document_path_provider_refusal=") +
                            detail);
  result.evidence.push_back("document_path_provider_fail_closed=true");
  result.evidence.push_back("document_path_provider_candidate_evidence_only=true");
  result.evidence.push_back("document_path_provider_finality_authority=false");
  result.evidence.push_back("document_path_provider_visibility_authority=false");
  result.evidence.push_back("parser_finality_authority=false");
  result.evidence.push_back("reference_finality_authority=false");
  result.evidence.push_back("write_ahead_log_finality_authority=false");  // wal-not-authority
  return result;
}

DocumentPathProviderProbeResult ProbeFailure(const char* detail) {
  DocumentPathProviderProbeResult result;
  result.ok = false;
  result.diagnostic = ProviderDiagnostic(detail);
  result.evidence.push_back(std::string("document_path_provider_refusal=") +
                            detail);
  result.evidence.push_back("document_path_provider_fail_closed=true");
  return result;
}

bool HasRequiredIdentity(const DocumentPathProviderIdentity& identity) {
  return IsValidUuid(identity.database_uuid) &&
         IsValidUuid(identity.relation_uuid) &&
         IsValidUuid(identity.index_uuid) &&
         IsValidUuid(identity.segment_uuid) &&
         !identity.provider_id.empty();
}

bool HasRequiredEpochs(const DocumentPathProviderIdentity& identity) {
  return identity.provider_generation != 0 && identity.catalog_epoch != 0 &&
         identity.security_epoch != 0 && identity.redaction_epoch != 0;
}

std::optional<const char*> ValidateIdentity(
    const DocumentPathProviderIdentity& identity) {
  if (!HasRequiredIdentity(identity)) {
    return kDocumentPathPhysicalProviderInvalidUuid;
  }
  if (!HasRequiredEpochs(identity)) {
    return kDocumentPathPhysicalProviderMissingGenerationEpoch;
  }
  return std::nullopt;
}

bool IdentityMatches(const DocumentPathProviderIdentity& actual,
                     const DocumentPathProviderIdentity& expected) {
  return actual.database_uuid == expected.database_uuid &&
         actual.relation_uuid == expected.relation_uuid &&
         actual.index_uuid == expected.index_uuid &&
         actual.provider_id == expected.provider_id &&
         actual.segment_uuid == expected.segment_uuid &&
         actual.catalog_epoch == expected.catalog_epoch &&
         actual.security_epoch == expected.security_epoch &&
         actual.redaction_epoch == expected.redaction_epoch;
}

PairList IdentityPairs(const DocumentPathProviderIdentity& identity) {
  return {
      {"database_uuid", identity.database_uuid},
      {"relation_uuid", identity.relation_uuid},
      {"index_uuid", identity.index_uuid},
      {"provider_id", identity.provider_id},
      {"segment_uuid", identity.segment_uuid},
      {"provider_generation", std::to_string(identity.provider_generation)},
      {"catalog_epoch", std::to_string(identity.catalog_epoch)},
      {"security_epoch", std::to_string(identity.security_epoch)},
      {"redaction_epoch", std::to_string(identity.redaction_epoch)},
  };
}

DocumentPathProviderIdentity IdentityFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderIdentity identity;
  identity.database_uuid = ValueOr(values, "database_uuid");
  identity.relation_uuid = ValueOr(values, "relation_uuid");
  identity.index_uuid = ValueOr(values, "index_uuid");
  identity.provider_id = ValueOr(values, "provider_id");
  identity.segment_uuid = ValueOr(values, "segment_uuid");
  identity.provider_generation = ParseU64(ValueOr(values, "provider_generation"));
  identity.catalog_epoch = ParseU64(ValueOr(values, "catalog_epoch"));
  identity.security_epoch = ParseU64(ValueOr(values, "security_epoch"));
  identity.redaction_epoch = ParseU64(ValueOr(values, "redaction_epoch"));
  return identity;
}

PairList PathPairs(const DocumentPathProviderPathEntry& entry) {
  return {{"path_id", std::to_string(entry.path_id)},
          {"path_kind", entry.path_kind},
          {"normalized_path", entry.normalized_path}};
}

DocumentPathProviderPathEntry PathFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderPathEntry entry;
  entry.path_id = ParseU64(ValueOr(values, "path_id"));
  entry.path_kind = ValueOr(values, "path_kind");
  entry.normalized_path = ValueOr(values, "normalized_path");
  return entry;
}

std::string JoinIds(const std::vector<std::uint64_t>& ids) {
  std::string out;
  for (const auto id : ids) {
    if (!out.empty()) { out.push_back(','); }
    out += std::to_string(id);
  }
  return out;
}

std::vector<std::uint64_t> ParseIds(const std::string& value) {
  std::vector<std::uint64_t> ids;
  for (const auto& part : Split(value, ',')) {
    const auto id = ParseU64(part);
    ids.push_back(id);
  }
  return ids;
}

PairList ShapePairs(const DocumentPathProviderShapeEntry& entry) {
  return {{"shape_id", std::to_string(entry.shape_id)},
          {"path_ids", JoinIds(entry.path_ids)}};
}

DocumentPathProviderShapeEntry ShapeFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderShapeEntry entry;
  entry.shape_id = ParseU64(ValueOr(values, "shape_id"));
  entry.path_ids = ParseIds(ValueOr(values, "path_ids"));
  return entry;
}

PairList PostingPairs(const DocumentPathProviderPosting& posting) {
  return {
      {"path_id", std::to_string(posting.path_id)},
      {"scalar_type", posting.scalar_type},
      {"encoded_value", posting.encoded_value},
      {"document_uuid", posting.document_uuid},
      {"row_uuid", posting.row_uuid},
      {"version_uuid", posting.version_uuid},
      {"row_ordinal", std::to_string(posting.row_ordinal)},
      {"concrete_path", posting.concrete_path},
      {"array_position", std::to_string(posting.array_position)},
      {"exact_recheck_required", "true"},
  };
}

DocumentPathProviderPosting PostingFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderPosting posting;
  posting.path_id = ParseU64(ValueOr(values, "path_id"));
  posting.scalar_type = ValueOr(values, "scalar_type");
  posting.encoded_value = ValueOr(values, "encoded_value");
  posting.document_uuid = ValueOr(values, "document_uuid");
  posting.row_uuid = ValueOr(values, "row_uuid");
  posting.version_uuid = ValueOr(values, "version_uuid");
  posting.row_ordinal = ParseU64(ValueOr(values, "row_ordinal"));
  posting.concrete_path = ValueOr(values, "concrete_path");
  posting.array_position = ParseI64(ValueOr(values, "array_position"));
  return posting;
}

PairList ArrayPairs(const DocumentPathProviderArrayExpansion& expansion) {
  return {
      {"wildcard_path_id", std::to_string(expansion.wildcard_path_id)},
      {"concrete_path_id", std::to_string(expansion.concrete_path_id)},
      {"concrete_path", expansion.concrete_path},
      {"array_position", std::to_string(expansion.array_position)},
  };
}

DocumentPathProviderArrayExpansion ArrayFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderArrayExpansion expansion;
  expansion.wildcard_path_id = ParseU64(ValueOr(values, "wildcard_path_id"));
  expansion.concrete_path_id = ParseU64(ValueOr(values, "concrete_path_id"));
  expansion.concrete_path = ValueOr(values, "concrete_path");
  expansion.array_position = ParseI64(ValueOr(values, "array_position"));
  return expansion;
}

PairList StatsPairs(const DocumentPathProviderStats& stats) {
  return {
      {"row_count", std::to_string(stats.row_count)},
      {"path_count", std::to_string(stats.path_count)},
      {"shape_count", std::to_string(stats.shape_count)},
      {"posting_count", std::to_string(stats.posting_count)},
      {"wildcard_path_count", std::to_string(stats.wildcard_path_count)},
      {"array_expansion_count", std::to_string(stats.array_expansion_count)},
  };
}

DocumentPathProviderStats StatsFromPairs(const PairList& pairs) {
  const auto values = PairMap(pairs);
  DocumentPathProviderStats stats;
  stats.row_count = ParseU64(ValueOr(values, "row_count"));
  stats.path_count = ParseU64(ValueOr(values, "path_count"));
  stats.shape_count = ParseU64(ValueOr(values, "shape_count"));
  stats.posting_count = ParseU64(ValueOr(values, "posting_count"));
  stats.wildcard_path_count = ParseU64(ValueOr(values, "wildcard_path_count"));
  stats.array_expansion_count =
      ParseU64(ValueOr(values, "array_expansion_count"));
  return stats;
}

bool PostingLess(const DocumentPathProviderPosting& lhs,
                 const DocumentPathProviderPosting& rhs) {
  return std::tie(lhs.path_id,
                  lhs.scalar_type,
                  lhs.encoded_value,
                  lhs.row_uuid,
                  lhs.version_uuid,
                  lhs.row_ordinal,
                  lhs.concrete_path) <
         std::tie(rhs.path_id,
                  rhs.scalar_type,
                  rhs.encoded_value,
                  rhs.row_uuid,
                  rhs.version_uuid,
                  rhs.row_ordinal,
                  rhs.concrete_path);
}

bool RowHasAuthorityClaim(const DocumentPathRowEvidence& row) {
  return row.summary_visibility_or_finality_claim ||
         row.parser_finality_authority_claim ||
         row.reference_finality_authority_claim ||
         row.provider_finality_authority_claim ||
         row.write_ahead_log_finality_authority_claim;  // wal-not-authority
}

std::uint64_t NonZeroEpoch(std::uint64_t epoch) { return epoch == 0 ? 1 : epoch; }

std::optional<const char*> ValidateRows(
    const std::vector<DocumentPathRowEvidence>& rows) {
  for (const auto& row : rows) {
    if (!row.authoritative_document_row_path_evidence) {
      return kDocumentPathPhysicalProviderRebuildRequired;
    }
    if (row.descriptor_scan_claim || row.behavior_scan_claim) {
      return kDocumentPathPhysicalProviderDescriptorScanRefused;
    }
    if (RowHasAuthorityClaim(row)) {
      return kDocumentPathPhysicalProviderAuthorityClaimRefused;
    }
    if (!IsValidUuid(row.document_uuid) || !IsValidUuid(row.row_uuid) ||
        !IsValidUuid(row.version_uuid)) {
      return kDocumentPathPhysicalProviderInvalidUuid;
    }
    for (const auto& value : row.values) {
      if (!IsSafePath(NormalizePath(value.path))) {
        return kDocumentPathPhysicalProviderUnsafePathToken;
      }
    }
  }
  return std::nullopt;
}

DocumentPathProviderArtifact BuildArtifact(
    const DocumentPathProviderIdentity& identity,
    const std::vector<DocumentPathRowEvidence>& rows) {
  DocumentPathProviderArtifact artifact;
  artifact.identity = identity;

  std::set<std::pair<std::string, std::string>> path_keys;
  std::map<std::pair<std::string, std::string>, std::uint64_t> path_ids;
  for (const auto& row : rows) {
    for (const auto& item : row.values) {
      const auto path = NormalizePath(item.path);
      path_keys.insert({"normalized", path});
      const auto wildcard = WildcardNormalizePath(path);
      if (wildcard != path || wildcard.find('*') != std::string::npos) {
        path_keys.insert({"wildcard", wildcard});
        path_keys.insert({"array_expanded", path});
      }
    }
  }
  std::uint64_t next_path_id = 1;
  for (const auto& key : path_keys) {
    path_ids[key] = next_path_id;
    artifact.path_dictionary.push_back(
        {next_path_id, key.first, key.second});
    ++next_path_id;
  }

  std::map<std::string, std::set<std::uint64_t>> shape_sets;
  std::map<std::string, std::uint64_t> shape_ids;
  for (const auto& row : rows) {
    std::vector<std::uint64_t> ids;
    for (const auto& item : row.values) {
      ids.push_back(path_ids[{"normalized", NormalizePath(item.path)}]);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    shape_sets[JoinIds(ids)] = std::set<std::uint64_t>(ids.begin(), ids.end());
  }
  std::uint64_t next_shape_id = 1;
  for (const auto& [shape_key, ids] : shape_sets) {
    (void)shape_key;
    shape_ids[shape_key] = next_shape_id;
    artifact.shape_dictionary.push_back(
        {next_shape_id, std::vector<std::uint64_t>(ids.begin(), ids.end())});
    ++next_shape_id;
  }

  for (const auto& row : rows) {
    for (const auto& item : row.values) {
      const auto path = NormalizePath(item.path);
      const auto wildcard = WildcardNormalizePath(path);
      const auto exact_id = path_ids[{"normalized", path}];
      const auto add_posting = [&](std::uint64_t path_id) {
        artifact.postings.push_back({path_id,
                                     item.value.is_null ? "null"
                                                        : item.value.scalar_type,
                                     item.value.is_null ? "" : item.value.encoded_value,
                                     row.document_uuid,
                                     row.row_uuid,
                                     row.version_uuid,
                                     row.row_ordinal,
                                     path,
                                     FirstArrayPosition(path)});
      };
      add_posting(exact_id);
      if (wildcard != path || wildcard.find('*') != std::string::npos) {
        const auto wildcard_id = path_ids[{"wildcard", wildcard}];
        add_posting(wildcard_id);
        const auto array_expanded_id = path_ids[{"array_expanded", path}];
        const auto array_position = FirstArrayPosition(path);
        if (array_position >= 0) {
          artifact.array_expansions.push_back(
              {wildcard_id, array_expanded_id, path, array_position});
        }
      }
    }
  }
  std::sort(artifact.postings.begin(), artifact.postings.end(), PostingLess);
  std::sort(artifact.array_expansions.begin(),
            artifact.array_expansions.end(),
            [](const auto& lhs, const auto& rhs) {
              return std::tie(lhs.wildcard_path_id,
                              lhs.concrete_path_id,
                              lhs.concrete_path,
                              lhs.array_position) <
                     std::tie(rhs.wildcard_path_id,
                              rhs.concrete_path_id,
                              rhs.concrete_path,
                              rhs.array_position);
            });
  artifact.array_expansions.erase(
      std::unique(artifact.array_expansions.begin(),
                  artifact.array_expansions.end(),
                  [](const auto& lhs, const auto& rhs) {
                    return std::tie(lhs.wildcard_path_id,
                                    lhs.concrete_path_id,
                                    lhs.concrete_path,
                                    lhs.array_position) ==
                           std::tie(rhs.wildcard_path_id,
                                    rhs.concrete_path_id,
                                    rhs.concrete_path,
                                    rhs.array_position);
                  }),
      artifact.array_expansions.end());

  artifact.stats.row_count = static_cast<std::uint64_t>(
      std::count_if(rows.begin(), rows.end(), [](const auto& row) {
        return !row.values.empty();
      }));
  artifact.stats.path_count = artifact.path_dictionary.size();
  artifact.stats.shape_count = artifact.shape_dictionary.size();
  artifact.stats.posting_count = artifact.postings.size();
  artifact.stats.wildcard_path_count = static_cast<std::uint64_t>(
      std::count_if(artifact.path_dictionary.begin(),
                    artifact.path_dictionary.end(),
                    [](const auto& entry) {
                      return entry.path_kind == "wildcard";
                    }));
  artifact.stats.array_expansion_count = artifact.array_expansions.size();
  return artifact;
}

std::string BodyForArtifact(const DocumentPathProviderArtifact& artifact) {
  std::ostringstream body;
  body << "IDENT\t" << EncodeCrudPairs(IdentityPairs(artifact.identity)) << '\n';
  for (const auto& path : artifact.path_dictionary) {
    body << "PATH\t" << EncodeCrudPairs(PathPairs(path)) << '\n';
  }
  for (const auto& shape : artifact.shape_dictionary) {
    body << "SHAPE\t" << EncodeCrudPairs(ShapePairs(shape)) << '\n';
  }
  for (const auto& posting : artifact.postings) {
    body << "POST\t" << EncodeCrudPairs(PostingPairs(posting)) << '\n';
  }
  for (const auto& expansion : artifact.array_expansions) {
    body << "ARRAY\t" << EncodeCrudPairs(ArrayPairs(expansion)) << '\n';
  }
  body << "STATS\t" << EncodeCrudPairs(StatsPairs(artifact.stats)) << '\n';
  body << "EVIDENCE\t"
       << EncodeCrudPairs({{"candidate_provider_evidence_only", "true"},
                           {"mga_security_redaction_exact_recheck_required",
                            "true"},
                           {"parser_finality_authority", "false"},
                           {"reference_finality_authority", "false"},
                           {"provider_finality_authority", "false"},
                           {"write_ahead_log_finality_authority", "false"}})  // wal-not-authority
       << '\n';
  return body.str();
}

std::string SerializeArtifact(const DocumentPathProviderArtifact& artifact) {
  const auto body = BodyForArtifact(artifact);
  std::ostringstream out;
  out << kMagic << '\n';
  out << "VERSION\t" << kFormatVersion << '\n';
  out << "CHECKSUM\t" << Hex64(Fnva64(body)) << '\n';
  out << body;
  out << "END\n";
  return out.str();
}

std::optional<const char*> ValidateParsedArtifact(
    const DocumentPathProviderArtifact& artifact) {
  if (auto invalid = ValidateIdentity(artifact.identity)) { return invalid; }

  std::set<std::uint64_t> path_ids;
  std::set<std::pair<std::string, std::string>> path_keys;
  std::map<std::uint64_t, DocumentPathProviderPathEntry> paths;
  for (const auto& path : artifact.path_dictionary) {
    if (path.path_id == 0 || !path_ids.insert(path.path_id).second ||
        (path.path_kind != "normalized" && path.path_kind != "wildcard" &&
         path.path_kind != "array_expanded")) {
      return kDocumentPathPhysicalProviderMalformedPathDictionary;
    }
    if (!path_keys.insert({path.path_kind, path.normalized_path}).second) {
      return kDocumentPathPhysicalProviderMalformedPathDictionary;
    }
    if (!IsSafePath(path.normalized_path)) {
      return kDocumentPathPhysicalProviderUnsafePathToken;
    }
    paths[path.path_id] = path;
  }

  std::set<std::uint64_t> shape_ids;
  std::set<std::string> shape_sets;
  for (const auto& shape : artifact.shape_dictionary) {
    if (shape.shape_id == 0 || !shape_ids.insert(shape.shape_id).second ||
        shape.path_ids.empty() ||
        !std::is_sorted(shape.path_ids.begin(), shape.path_ids.end()) ||
        std::adjacent_find(shape.path_ids.begin(), shape.path_ids.end()) !=
            shape.path_ids.end()) {
      return kDocumentPathPhysicalProviderMalformedShapeDictionary;
    }
    for (const auto path_id : shape.path_ids) {
      if (paths.find(path_id) == paths.end()) {
        return kDocumentPathPhysicalProviderMalformedShapeDictionary;
      }
    }
    if (!shape_sets.insert(JoinIds(shape.path_ids)).second) {
      return kDocumentPathPhysicalProviderMalformedShapeDictionary;
    }
  }

  std::set<std::tuple<std::uint64_t,
                      std::string,
                      std::string,
                      std::string,
                      std::string,
                      std::string,
                      std::uint64_t,
                      std::string,
                      std::int64_t>>
      posting_keys;
  std::set<std::tuple<std::string, std::string, std::uint64_t>> row_keys;
  for (const auto& posting : artifact.postings) {
    const auto path_it = paths.find(posting.path_id);
    if (posting.path_id == 0 || path_it == paths.end() ||
        posting.scalar_type.empty() || !IsValidUuid(posting.document_uuid) ||
        !IsValidUuid(posting.row_uuid) || !IsValidUuid(posting.version_uuid) ||
        !IsSafePath(posting.concrete_path) ||
        posting.array_position != FirstArrayPosition(posting.concrete_path) ||
        (path_it->second.path_kind != "normalized" &&
         path_it->second.path_kind != "wildcard")) {
      return kDocumentPathPhysicalProviderMalformedPostings;
    }
    if ((path_it->second.path_kind == "normalized" &&
         path_it->second.normalized_path != NormalizePath(posting.concrete_path)) ||
        (path_it->second.path_kind == "wildcard" &&
         path_it->second.normalized_path !=
             WildcardNormalizePath(posting.concrete_path))) {
      return kDocumentPathPhysicalProviderMalformedPostings;
    }
    if (!posting_keys
             .insert({posting.path_id,
                      posting.scalar_type,
                      posting.encoded_value,
                      posting.document_uuid,
                      posting.row_uuid,
                      posting.version_uuid,
                      posting.row_ordinal,
                      posting.concrete_path,
                      posting.array_position})
             .second) {
      return kDocumentPathPhysicalProviderMalformedPostings;
    }
    row_keys.insert(
        {posting.row_uuid, posting.version_uuid, posting.row_ordinal});
  }

  std::set<std::tuple<std::uint64_t, std::uint64_t, std::string, std::int64_t>>
      expansion_keys;
  for (const auto& expansion : artifact.array_expansions) {
    const auto wildcard = paths.find(expansion.wildcard_path_id);
    const auto concrete = paths.find(expansion.concrete_path_id);
    if (wildcard == paths.end() || concrete == paths.end() ||
        wildcard->second.path_kind != "wildcard" ||
        concrete->second.path_kind != "array_expanded" ||
        !IsSafePath(expansion.concrete_path) ||
        expansion.array_position < 0 ||
        concrete->second.normalized_path != NormalizePath(expansion.concrete_path) ||
        wildcard->second.normalized_path !=
            WildcardNormalizePath(expansion.concrete_path) ||
        !expansion_keys
             .insert({expansion.wildcard_path_id,
                      expansion.concrete_path_id,
                      expansion.concrete_path,
                      expansion.array_position})
             .second) {
      return kDocumentPathPhysicalProviderMalformedPostings;
    }
  }

  const auto computed_wildcards = static_cast<std::uint64_t>(
      std::count_if(artifact.path_dictionary.begin(),
                    artifact.path_dictionary.end(),
                    [](const auto& entry) {
                      return entry.path_kind == "wildcard";
                    }));
  if (artifact.stats.row_count != row_keys.size() ||
      artifact.stats.path_count != artifact.path_dictionary.size() ||
      artifact.stats.shape_count != artifact.shape_dictionary.size() ||
      artifact.stats.posting_count != artifact.postings.size() ||
      artifact.stats.wildcard_path_count != computed_wildcards ||
      artifact.stats.array_expansion_count != artifact.array_expansions.size()) {
    return kDocumentPathPhysicalProviderMalformedPostings;
  }
  return std::nullopt;
}

DocumentPathProviderResult ParseArtifactText(
    const std::string& text,
    const DocumentPathProviderOpenRequest& request) {
  const auto first_newline = text.find('\n');
  if (first_newline == std::string::npos ||
      text.substr(0, first_newline) != kMagic) {
    return Failure(kDocumentPathPhysicalProviderStaleFormat);
  }
  if (text.size() < 4 || text.rfind("END\n") != text.size() - 4) {
    return Failure(kDocumentPathPhysicalProviderTruncatedPayload);
  }

  std::istringstream lines(text);
  std::string line;
  std::getline(lines, line);
  std::getline(lines, line);
  if (line != "VERSION\t1") {
    return Failure(kDocumentPathPhysicalProviderStaleFormat);
  }
  std::getline(lines, line);
  if (line.rfind("CHECKSUM\t", 0) != 0) {
    return Failure(kDocumentPathPhysicalProviderBadChecksum);
  }
  const auto expected_checksum = line.substr(9);

  std::string body;
  while (std::getline(lines, line)) {
    if (line == "END") { break; }
    body += line;
    body.push_back('\n');
  }
  if (Hex64(Fnva64(body)) != expected_checksum) {
    return Failure(kDocumentPathPhysicalProviderBadChecksum);
  }

  DocumentPathProviderArtifact artifact;
  bool saw_ident = false;
  bool saw_stats = false;
  std::istringstream body_lines(body);
  while (std::getline(body_lines, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) { continue; }
    const auto tag = line.substr(0, tab);
    const auto pairs = DecodeCrudPairs(line.substr(tab + 1));
    if (tag == "IDENT") {
      artifact.identity = IdentityFromPairs(pairs);
      saw_ident = true;
    } else if (tag == "PATH") {
      artifact.path_dictionary.push_back(PathFromPairs(pairs));
    } else if (tag == "SHAPE") {
      artifact.shape_dictionary.push_back(ShapeFromPairs(pairs));
    } else if (tag == "POST") {
      artifact.postings.push_back(PostingFromPairs(pairs));
    } else if (tag == "ARRAY") {
      artifact.array_expansions.push_back(ArrayFromPairs(pairs));
    } else if (tag == "STATS") {
      artifact.stats = StatsFromPairs(pairs);
      saw_stats = true;
    }
  }
  if (!saw_ident || !saw_stats) {
    return Failure(kDocumentPathPhysicalProviderTruncatedPayload);
  }
  if (auto invalid = ValidateParsedArtifact(artifact)) {
    return Failure(*invalid);
  }
  if (request.require_expected_identity) {
    if (!IdentityMatches(artifact.identity, request.expected_identity)) {
      return Failure(kDocumentPathPhysicalProviderIdentityMismatch);
    }
    if (artifact.identity.provider_generation !=
        request.expected_identity.provider_generation) {
      return Failure(kDocumentPathPhysicalProviderStaleGeneration);
    }
  }

  DocumentPathProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.artifact = std::move(artifact);
  result.evidence.push_back("document_path_provider_opened=true");
  result.evidence.push_back("document_path_provider_checksum_valid=true");
  result.evidence.push_back("document_path_provider_candidate_evidence_only=true");
  result.evidence.push_back("mga_security_redaction_exact_recheck_required=true");
  result.evidence.push_back("parser_finality_authority=false");
  result.evidence.push_back("reference_finality_authority=false");
  result.evidence.push_back("provider_finality_authority=false");
  result.evidence.push_back("write_ahead_log_finality_authority=false");  // wal-not-authority
  return result;
}

std::vector<DocumentPathRowEvidence> RowsFromArtifact(
    const DocumentPathProviderArtifact& artifact) {
  std::map<std::tuple<std::string, std::string, std::uint64_t>,
           DocumentPathRowEvidence>
      rows;
  std::map<std::uint64_t, DocumentPathProviderPathEntry> paths;
  for (const auto& path : artifact.path_dictionary) {
    paths[path.path_id] = path;
  }
  for (const auto& posting : artifact.postings) {
    const auto path_it = paths.find(posting.path_id);
    if (path_it == paths.end() || path_it->second.path_kind != "normalized") {
      continue;
    }
    auto& row = rows[{posting.row_uuid, posting.version_uuid, posting.row_ordinal}];
    row.document_uuid = posting.document_uuid;
    row.row_uuid = posting.row_uuid;
    row.version_uuid = posting.version_uuid;
    row.row_ordinal = posting.row_ordinal;
    row.values.push_back({posting.concrete_path,
                          {posting.scalar_type,
                           posting.encoded_value,
                           posting.scalar_type == "null"}});
  }
  std::vector<DocumentPathRowEvidence> out;
  for (auto& [key, row] : rows) {
    (void)key;
    std::sort(row.values.begin(),
              row.values.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.path < rhs.path;
              });
    out.push_back(std::move(row));
  }
  return out;
}

std::optional<std::uint64_t> FindPathId(
    const DocumentPathProviderArtifact& artifact,
    const std::string& path_kind,
    const std::string& normalized_path) {
  for (const auto& entry : artifact.path_dictionary) {
    if (entry.path_kind == path_kind && entry.normalized_path == normalized_path) {
      return entry.path_id;
    }
  }
  return std::nullopt;
}

std::pair<std::uint64_t, std::uint64_t> ShapeInfoForCandidate(
    const DocumentPathProviderArtifact& artifact,
    const DocumentPathProviderPosting& candidate) {
  std::vector<std::uint64_t> candidate_path_ids;
  std::map<std::uint64_t, DocumentPathProviderPathEntry> paths;
  for (const auto& path : artifact.path_dictionary) {
    paths[path.path_id] = path;
  }
  for (const auto& posting : artifact.postings) {
    if (posting.row_uuid != candidate.row_uuid ||
        posting.version_uuid != candidate.version_uuid ||
        posting.row_ordinal != candidate.row_ordinal) {
      continue;
    }
    const auto path = paths.find(posting.path_id);
    if (path != paths.end() && path->second.path_kind == "normalized") {
      candidate_path_ids.push_back(posting.path_id);
    }
  }
  std::sort(candidate_path_ids.begin(), candidate_path_ids.end());
  candidate_path_ids.erase(
      std::unique(candidate_path_ids.begin(), candidate_path_ids.end()),
      candidate_path_ids.end());

  std::uint64_t shape_id = 0;
  for (const auto& shape : artifact.shape_dictionary) {
    if (shape.path_ids == candidate_path_ids) {
      shape_id = shape.shape_id;
      break;
    }
  }
  if (shape_id == 0) {
    return {0, 0};
  }

  std::set<std::tuple<std::string, std::string, std::uint64_t>> matching_rows;
  for (const auto& posting : artifact.postings) {
    const auto path = paths.find(posting.path_id);
    if (path == paths.end() || path->second.path_kind != "normalized") {
      continue;
    }
    std::vector<std::uint64_t> row_path_ids;
    for (const auto& row_posting : artifact.postings) {
      if (row_posting.row_uuid != posting.row_uuid ||
          row_posting.version_uuid != posting.version_uuid ||
          row_posting.row_ordinal != posting.row_ordinal) {
        continue;
      }
      const auto row_path = paths.find(row_posting.path_id);
      if (row_path != paths.end() &&
          row_path->second.path_kind == "normalized") {
        row_path_ids.push_back(row_posting.path_id);
      }
    }
    std::sort(row_path_ids.begin(), row_path_ids.end());
    row_path_ids.erase(std::unique(row_path_ids.begin(), row_path_ids.end()),
                       row_path_ids.end());
    if (row_path_ids == candidate_path_ids) {
      matching_rows.insert(
          {posting.row_uuid, posting.version_uuid, posting.row_ordinal});
    }
  }
  return {shape_id, static_cast<std::uint64_t>(matching_rows.size())};
}

std::vector<DocumentPathProviderProjectedValue> ProjectValues(
    const DocumentPathProviderArtifact& artifact,
    const DocumentPathProviderPosting& candidate,
    const std::vector<std::string>& requested_paths,
    bool wildcard_path,
    const std::string& probe_path) {
  std::vector<DocumentPathProviderProjectedValue> projected;
  std::set<std::string> wanted;
  for (const auto& path : requested_paths) {
    wanted.insert(NormalizePath(path));
  }
  for (const auto& posting : artifact.postings) {
    if (posting.row_uuid != candidate.row_uuid ||
        posting.version_uuid != candidate.version_uuid ||
        posting.row_ordinal != candidate.row_ordinal) {
      continue;
    }
    const auto path_entry =
        std::find_if(artifact.path_dictionary.begin(),
                     artifact.path_dictionary.end(),
                     [&](const auto& entry) {
                       return entry.path_id == posting.path_id;
                     });
    if (path_entry == artifact.path_dictionary.end() ||
        path_entry->path_kind != "normalized") {
      continue;
    }
    const bool requested = !wanted.empty() && wanted.count(posting.concrete_path) != 0;
    const bool matched_probe = wanted.empty() &&
                               ((wildcard_path &&
                                 WildcardNormalizePath(posting.concrete_path) ==
                                     NormalizePath(probe_path)) ||
                                (!wildcard_path &&
                                 posting.concrete_path == NormalizePath(probe_path)));
    if (!requested && !matched_probe) { continue; }
    projected.push_back({posting.concrete_path,
                         {posting.scalar_type,
                          posting.encoded_value,
                          posting.scalar_type == "null"}});
  }
  return projected;
}

bool SameCandidate(const DocumentPathProviderCandidate& lhs,
                   const DocumentPathProviderPosting& rhs) {
  return lhs.row_uuid == rhs.row_uuid && lhs.version_uuid == rhs.version_uuid &&
         lhs.row_ordinal == rhs.row_ordinal;
}

void AddSuccessEvidence(DocumentPathProviderResult* result) {
  result->evidence.push_back("document_path_provider_persisted=true");
  result->evidence.push_back("document_path_provider_magic=SBDOCPATH");
  result->evidence.push_back("document_path_provider_version=1");
  result->evidence.push_back("document_path_provider_checksum_present=true");
  result->evidence.push_back("document_path_provider_candidate_evidence_only=true");
  result->evidence.push_back("mga_security_redaction_exact_recheck_required=true");
  result->evidence.push_back("parser_finality_authority=false");
  result->evidence.push_back("reference_finality_authority=false");
  result->evidence.push_back("provider_finality_authority=false");
  result->evidence.push_back("write_ahead_log_finality_authority=false");  // wal-not-authority
}

}  // namespace

std::string DocumentPathPhysicalProviderPath(
    const EngineRequestContext& context) {
  if (context.database_path.empty()) { return {}; }
  return context.database_path + ".sb.document_path_provider";
}

DocumentPathProviderIdentity DocumentPathProviderIdentityForContext(
    const EngineRequestContext& context,
    std::uint64_t provider_generation,
    const std::string& index_uuid) {
  DocumentPathProviderIdentity identity;
  const bool database_uuid_valid = IsValidUuid(context.database_uuid.canonical);
  const bool relation_uuid_valid =
      IsValidUuid(context.current_schema_uuid.canonical);
  std::string database_seed;
  if (database_uuid_valid) {
    database_seed = context.database_uuid.canonical;
  } else if (!context.database_path.empty()) {
    database_seed = context.database_path;
  } else if (!context.database_uuid.canonical.empty()) {
    database_seed = context.database_uuid.canonical;
  } else {
    database_seed = "embedded_transient_database";
  }
  identity.database_uuid = database_uuid_valid
                               ? context.database_uuid.canonical
                               : StableProviderUuid(
                                     scratchbird::core::platform::UuidKind::database,
                                     database_seed + "|database");
  identity.relation_uuid =
      relation_uuid_valid
          ? context.current_schema_uuid.canonical
          : StableProviderUuid(
                scratchbird::core::platform::UuidKind::object,
                database_seed + "|document_relation");
  identity.index_uuid =
      !index_uuid.empty() && IsValidUuid(index_uuid)
          ? index_uuid
          : StableProviderUuid(
                scratchbird::core::platform::UuidKind::object,
                database_seed + "|" + identity.relation_uuid +
                    "|document_path_index");
  identity.provider_id = kDocumentPathPhysicalProviderId;
  identity.segment_uuid =
      StableProviderUuid(
          scratchbird::core::platform::UuidKind::object,
          database_seed + "|" + identity.relation_uuid + "|" +
              identity.index_uuid + "|document_path_segment");
  identity.provider_generation = provider_generation;
  identity.catalog_epoch = NonZeroEpoch(context.catalog_generation_id);
  identity.security_epoch = NonZeroEpoch(context.security_epoch);
  identity.redaction_epoch = NonZeroEpoch(context.security_epoch);
  return identity;
}

DocumentPathScalar DocumentPathScalarFromTypedValue(
    const EngineTypedValue& value) {
  DocumentPathScalar scalar;
  scalar.encoded_value = value.encoded_value;
  scalar.is_null = value.is_null;
  if (value.is_null) {
    scalar.scalar_type = "null";
  } else if (!value.descriptor.canonical_type_name.empty()) {
    scalar.scalar_type = value.descriptor.canonical_type_name;
  } else {
    scalar.scalar_type = "string";
  }
  return scalar;
}

DocumentPathProviderResult BuildDocumentPathPhysicalProvider(
    const DocumentPathProviderBuildRequest& request) {
  if (auto invalid = ValidateIdentity(request.identity)) {
    return Failure(*invalid);
  }
  if (auto invalid = ValidateRows(request.rows)) {
    return Failure(*invalid);
  }
  auto artifact = BuildArtifact(request.identity, request.rows);
  if (auto invalid = ValidateParsedArtifact(artifact)) {
    return Failure(*invalid);
  }
  if (request.artifact_path.empty()) {
    return Failure(kDocumentPathPhysicalProviderIdentityMismatch);
  }
  std::ofstream out(request.artifact_path, std::ios::binary | std::ios::trunc);
  if (!out) { return Failure(kDocumentPathPhysicalProviderIdentityMismatch); }
  out << SerializeArtifact(artifact);
  out.flush();
  if (!out) { return Failure(kDocumentPathPhysicalProviderIdentityMismatch); }

  DocumentPathProviderResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.artifact = std::move(artifact);
  AddSuccessEvidence(&result);
  result.evidence.push_back("path_dictionary_entries=" +
                            std::to_string(result.artifact.stats.path_count));
  result.evidence.push_back("shape_dictionary_entries=" +
                            std::to_string(result.artifact.stats.shape_count));
  result.evidence.push_back("typed_path_value_postings=" +
                            std::to_string(result.artifact.stats.posting_count));
  result.evidence.push_back("wildcard_path_entries=" +
                            std::to_string(result.artifact.stats.wildcard_path_count));
  result.evidence.push_back("array_expansion_entries=" +
                            std::to_string(result.artifact.stats.array_expansion_count));
  return result;
}

DocumentPathProviderResult AppendDocumentPathPhysicalProvider(
    const DocumentPathProviderAppendRequest& request) {
  std::vector<DocumentPathRowEvidence> rows;
  std::uint64_t next_generation = request.next_identity.provider_generation;
  if (std::filesystem::exists(request.artifact_path)) {
    DocumentPathProviderOpenRequest open;
    open.artifact_path = request.artifact_path;
    auto opened = OpenDocumentPathPhysicalProvider(open);
    if (!opened.ok) { return opened; }
    rows = RowsFromArtifact(opened.artifact);
    next_generation = opened.artifact.identity.provider_generation + 1;
  } else if (next_generation == 0) {
    next_generation = 1;
  }
  rows.push_back(request.row);
  auto identity = request.next_identity;
  identity.provider_generation = next_generation;
  DocumentPathProviderBuildRequest build;
  build.artifact_path = request.artifact_path;
  build.identity = std::move(identity);
  build.rows = std::move(rows);
  auto result = BuildDocumentPathPhysicalProvider(build);
  if (result.ok) {
    result.evidence.push_back("document_path_provider_append=true");
    result.evidence.push_back("document_path_provider_delete_update_rebuild_required=true");
  }
  return result;
}

DocumentPathProviderResult OpenDocumentPathPhysicalProvider(
    const DocumentPathProviderOpenRequest& request) {
  std::ifstream in(request.artifact_path, std::ios::binary);
  if (!in) {
    if (!request.authoritative_source_rows.empty() && !request.repair_admitted) {
      return Failure(kDocumentPathPhysicalProviderRepairAdmissionRequired);
    }
    if (request.repair_admitted) {
      if (request.authoritative_source_rows.empty()) {
        return Failure(kDocumentPathPhysicalProviderRepairSourceRequired);
      }
      DocumentPathProviderBuildRequest rebuild;
      rebuild.artifact_path = request.artifact_path;
      rebuild.identity = request.expected_identity;
      rebuild.rows = request.authoritative_source_rows;
      auto repaired = BuildDocumentPathPhysicalProvider(rebuild);
      if (repaired.ok) {
        repaired.evidence.push_back("document_path_provider_repair_admitted=true");
        repaired.evidence.push_back(
            "document_path_provider_repair_source=authoritative_rows");
        repaired.evidence.push_back(
            "document_path_provider_descriptor_scan_fallback=false");
      }
      return repaired;
    }
    return Failure(kDocumentPathPhysicalProviderIdentityMismatch);
  }
  std::ostringstream text;
  text << in.rdbuf();
  auto result = ParseArtifactText(text.str(), request);
  if (!result.ok && request.repair_admitted) {
    if (request.authoritative_source_rows.empty()) {
      return Failure(kDocumentPathPhysicalProviderRepairSourceRequired);
    }
    DocumentPathProviderBuildRequest rebuild;
    rebuild.artifact_path = request.artifact_path;
    rebuild.identity = request.expected_identity;
    rebuild.rows = request.authoritative_source_rows;
    auto repaired = BuildDocumentPathPhysicalProvider(rebuild);
    if (repaired.ok) {
      repaired.evidence.push_back("document_path_provider_repair_admitted=true");
      repaired.evidence.push_back(
          "document_path_provider_repair_source=authoritative_rows");
      repaired.evidence.push_back("document_path_provider_descriptor_scan_fallback=false");
    }
    return repaired;
  }
  if (!result.ok && !request.authoritative_source_rows.empty()) {
    return Failure(kDocumentPathPhysicalProviderRepairAdmissionRequired);
  }
  return result;
}

DocumentPathProviderResult DeleteOrUpdateDocumentPathPhysicalProvider(
    const DocumentPathProviderMutationRequest& request) {
  if (!request.admitted_authoritative_rebuild) {
    return Failure(kDocumentPathPhysicalProviderRebuildRequired);
  }
  DocumentPathProviderOpenRequest open;
  open.artifact_path = request.artifact_path;
  auto opened = OpenDocumentPathPhysicalProvider(open);
  if (!opened.ok) { return opened; }
  auto identity = opened.artifact.identity;
  ++identity.provider_generation;
  DocumentPathProviderBuildRequest rebuild;
  rebuild.artifact_path = request.artifact_path;
  rebuild.identity = std::move(identity);
  rebuild.rows = request.authoritative_source_rows;
  auto rebuilt = BuildDocumentPathPhysicalProvider(rebuild);
  if (rebuilt.ok) {
    rebuilt.evidence.push_back("document_path_provider_authoritative_rebuild=true");
    rebuilt.evidence.push_back("document_path_provider_delete_update_rebuilt=true");
  }
  return rebuilt;
}

DocumentPathProviderProbeResult ProbeDocumentPathPhysicalProvider(
    const DocumentPathProviderProbeRequest& request) {
  if (!IsSafePath(NormalizePath(request.path))) {
    return ProbeFailure(kDocumentPathPhysicalProviderUnsafePathToken);
  }
  if (!request.require_expected_identity &&
      !std::filesystem::exists(request.artifact_path)) {
    DocumentPathProviderProbeResult result;
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    result.projection_plan.fetch_candidate_rows_only = true;
    result.projection_plan.fetch_projected_paths_only = !request.fetch_full_payload;
    result.projection_plan.fetch_full_payload = request.fetch_full_payload;
    result.projection_plan.projected_paths = request.projected_paths;
    result.evidence.push_back("document_path_provider_missing_probe_empty=true");
    result.evidence.push_back("document_path_provider_candidate_evidence_only=true");
    result.evidence.push_back("descriptor_scan_selected=false");
    result.evidence.push_back("behavior_store_scan_selected=false");
    return result;
  }
  DocumentPathProviderOpenRequest open;
  open.artifact_path = request.artifact_path;
  open.expected_identity = request.expected_identity;
  open.require_expected_identity = request.require_expected_identity;
  auto opened = OpenDocumentPathPhysicalProvider(open);
  if (!opened.ok) {
    DocumentPathProviderProbeResult failure;
    failure.ok = false;
    failure.diagnostic = opened.diagnostic;
    failure.evidence = opened.evidence;
    return failure;
  }

  const auto normalized_probe = NormalizePath(request.path);
  const auto path_id = FindPathId(opened.artifact,
                                  request.wildcard_path ? "wildcard" : "normalized",
                                  normalized_probe);
  DocumentPathProviderProbeResult result;
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.stats = opened.artifact.stats;
  result.index_probes = 1;
  result.projection_plan.fetch_candidate_rows_only = true;
  result.projection_plan.fetch_projected_paths_only = !request.fetch_full_payload;
  result.projection_plan.fetch_full_payload = request.fetch_full_payload;
  result.projection_plan.projected_paths = request.projected_paths;
  result.evidence = opened.evidence;
  result.evidence.push_back("document_path_provider_index_consumed=true");
  result.evidence.push_back(
      request.wildcard_path
          ? "document_path_provider_wildcard_posting_map_consumed=true"
          : "document_path_provider_value_posting_map_consumed=true");
  result.evidence.push_back("descriptor_scan_selected=false");
  result.evidence.push_back("behavior_store_scan_selected=false");
  result.evidence.push_back("exact_recheck_required=true");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  result.evidence.push_back("redaction_recheck_required=true");
  result.evidence.push_back("projection_fetch_candidate_rows_only=true");
  result.evidence.push_back(std::string("projection_fetch_full_payload=") +
                            BoolText(request.fetch_full_payload));
  if (!path_id) {
    result.evidence.push_back("document_path_provider_missing_probe_empty=true");
    return result;
  }

  std::vector<DocumentPathProviderPosting> candidates;
  const bool has_equals_filter =
      request.equals_value.is_null || !request.equals_value.encoded_value.empty();
  const auto key = PostingKey(*path_id,
                              request.equals_value.is_null
                                  ? "null"
                                  : request.equals_value.scalar_type,
                              request.equals_value.is_null
                                  ? ""
                                  : request.equals_value.encoded_value);
  for (const auto& posting : opened.artifact.postings) {
    const bool matches_path = posting.path_id == *path_id;
    const bool matches_value =
        !has_equals_filter ||
        PostingKey(posting.path_id, posting.scalar_type, posting.encoded_value) ==
            key;
    if (matches_path && matches_value) {
      candidates.push_back(posting);
    }
  }
  std::sort(candidates.begin(), candidates.end(), PostingLess);
  for (const auto& posting : candidates) {
    auto duplicate =
        std::find_if(result.projection_plan.candidates.begin(),
                     result.projection_plan.candidates.end(),
                     [&](const auto& candidate) {
                       return SameCandidate(candidate, posting);
                     });
    if (duplicate != result.projection_plan.candidates.end()) { continue; }
    DocumentPathProviderCandidate candidate;
    candidate.document_uuid = posting.document_uuid;
    candidate.row_uuid = posting.row_uuid;
    candidate.version_uuid = posting.version_uuid;
    candidate.row_ordinal = posting.row_ordinal;
    const auto shape_info = ShapeInfoForCandidate(opened.artifact, posting);
    if (shape_info.first != 0) {
      candidate.shape_id = "document_shape_" + std::to_string(shape_info.first);
      candidate.shape_ref_count = shape_info.second;
    }
    candidate.projected_values = ProjectValues(opened.artifact,
                                               posting,
                                               request.projected_paths,
                                               request.wildcard_path,
                                               request.path);
    result.projection_plan.candidates.push_back(std::move(candidate));
  }
  result.evidence.push_back("document_path_provider_candidates=" +
                            std::to_string(result.projection_plan.candidates.size()));
  result.evidence.push_back("array_expansion_entries=" +
                            std::to_string(opened.artifact.stats.array_expansion_count));
  if (request.wildcard_path && opened.artifact.stats.array_expansion_count > 0) {
    result.evidence.push_back(
        "document_path_provider_array_expansion_map_consumed=true");
  }
  return result;
}

bool DocumentPathProviderEvidenceContains(
    const std::vector<std::string>& evidence,
    const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

}  // namespace scratchbird::engine::internal_api
