// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resource_seed_pack.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::core::resources {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ResourceSeedOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status ResourceSeedErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

struct ExpectedArtifact {
  std::string content_hash;
  u64 content_size_bytes = 0;
};

ResourceSeedCatalogImageResult ResourceSeedError(std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {}) {
  ResourceSeedCatalogImageResult result;
  result.status = ResourceSeedErrorStatus();
  result.diagnostic = MakeResourceSeedDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

ResourceSeedAliasResolutionResult ResourceSeedAliasError(std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail = {}) {
  ResourceSeedAliasResolutionResult result;
  result.status = ResourceSeedErrorStatus();
  result.diagnostic = MakeResourceSeedDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

ResourceSeedLifecycleEvaluationResult ResourceSeedLifecycleOk() {
  ResourceSeedLifecycleEvaluationResult result;
  result.status = ResourceSeedOkStatus();
  return result;
}

ResourceSeedLifecycleEvaluationResult ResourceSeedLifecycleError(std::string diagnostic_code,
                                                                std::string message_key,
                                                                std::string detail = {}) {
  ResourceSeedLifecycleEvaluationResult result;
  result.status = ResourceSeedErrorStatus();
  result.diagnostic = MakeResourceSeedDiagnostic(result.status,
                                                 std::move(diagnostic_code),
                                                 std::move(message_key),
                                                 std::move(detail));
  return result;
}

std::string Trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::vector<std::string> SplitSemicolon(const std::string& value) {
  std::vector<std::string> result;
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, ';')) {
    part = Trim(part);
    if (!part.empty()) {
      result.push_back(part);
    }
  }
  return result;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ParseU64(const std::string& value, u64* parsed) {
  if (value.empty() || parsed == nullptr) {
    return false;
  }
  char* end = nullptr;
  const unsigned long long raw = std::strtoull(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  *parsed = static_cast<u64>(raw);
  return true;
}

std::vector<std::string> ParseCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (char ch : line) {
    if (ch == '"') {
      quoted = !quoted;
      continue;
    }
    if (ch == ',' && !quoted) {
      fields.push_back(Trim(field));
      field.clear();
      continue;
    }
    field.push_back(ch);
  }
  fields.push_back(Trim(field));
  return fields;
}

ResourceSeedCatalogImageResult LoadExpectedArtifactIndex(const std::filesystem::path& root,
                                                         std::map<std::string, ExpectedArtifact>* expected) {
  const std::filesystem::path index_path = root / "RESOURCE_SEED_ARTIFACTS.csv";
  if (!std::filesystem::is_regular_file(index_path)) {
    return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                             "resource.seed_pack.artifact_index_missing",
                             index_path.string());
  }

  std::ifstream index(index_path);
  if (!index) {
    return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                             "resource.seed_pack.artifact_index_unreadable",
                             index_path.string());
  }

  std::string line;
  bool header = true;
  while (std::getline(index, line)) {
    if (header) {
      header = false;
      continue;
    }
    if (Trim(line).empty()) {
      continue;
    }

    const std::vector<std::string> fields = ParseCsvLine(line);
    if (fields.size() < 3) {
      return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                               "resource.seed_pack.artifact_index_row_invalid",
                               line);
    }

    ExpectedArtifact artifact;
    artifact.content_hash = fields[1];
    if (!ParseU64(fields[2], &artifact.content_size_bytes)) {
      return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                               "resource.seed_pack.artifact_index_size_invalid",
                               line);
    }
    (*expected)[fields[0]] = std::move(artifact);
  }

  ResourceSeedCatalogImageResult result;
  result.status = ResourceSeedOkStatus();
  return result;
}

ResourceSeedFamily ParseFamily(const std::string& value) {
  if (value == "charset") { return ResourceSeedFamily::charset; }
  if (value == "charset_mapping") { return ResourceSeedFamily::charset_mapping; }
  if (value == "charset_mapping_schema") { return ResourceSeedFamily::charset_mapping_schema; }
  if (value == "collation") { return ResourceSeedFamily::collation; }
  if (value == "locale") { return ResourceSeedFamily::locale; }
  if (value == "uca") { return ResourceSeedFamily::uca; }
  if (value == "uca_manifest") { return ResourceSeedFamily::uca_manifest; }
  if (value == "i18n_version") { return ResourceSeedFamily::i18n_version; }
  if (value == "timezone_version") { return ResourceSeedFamily::timezone_version; }
  if (value == "timezone_source") { return ResourceSeedFamily::timezone_source; }
  if (value == "timezone_tables") { return ResourceSeedFamily::timezone_tables; }
  if (value == "timezone_leaps") { return ResourceSeedFamily::timezone_leaps; }
  if (value == "timezone_archives") { return ResourceSeedFamily::timezone_archives; }
  if (value == "sbsql_language_resource_pack") { return ResourceSeedFamily::sbsql_language_resource_pack; }
  if (value == "sbsql_language_resource_pack_artifacts") { return ResourceSeedFamily::sbsql_language_resource_pack_artifacts; }
  if (value == "sbsql_language_resource_pack_provenance") { return ResourceSeedFamily::sbsql_language_resource_pack_provenance; }
  return ResourceSeedFamily::unknown;
}

ResourceSeedFamily CanonicalVersionFamily(ResourceSeedFamily family) {
  switch (family) {
    case ResourceSeedFamily::charset:
    case ResourceSeedFamily::charset_mapping:
    case ResourceSeedFamily::charset_mapping_schema:
      return ResourceSeedFamily::charset;
    case ResourceSeedFamily::collation:
    case ResourceSeedFamily::uca:
    case ResourceSeedFamily::uca_manifest:
      return ResourceSeedFamily::collation;
    case ResourceSeedFamily::locale:
      return ResourceSeedFamily::locale;
    case ResourceSeedFamily::timezone_version:
    case ResourceSeedFamily::timezone_source:
    case ResourceSeedFamily::timezone_tables:
    case ResourceSeedFamily::timezone_leaps:
    case ResourceSeedFamily::timezone_archives:
      return ResourceSeedFamily::timezone_version;
    case ResourceSeedFamily::sbsql_language_resource_pack:
    case ResourceSeedFamily::sbsql_language_resource_pack_artifacts:
    case ResourceSeedFamily::sbsql_language_resource_pack_provenance:
      return ResourceSeedFamily::i18n_version;
    case ResourceSeedFamily::i18n_version:
    case ResourceSeedFamily::unknown:
      return family;
  }
  return ResourceSeedFamily::unknown;
}

std::string MakeDerivedFamilyVersion(const char* family_name,
                                     const std::string& base_version,
                                     const std::string& content_hash) {
  if (base_version.empty() || content_hash.empty()) {
    return {};
  }
  return std::string(family_name) + ":" + base_version + ":" + content_hash;
}

void AppendFamilyHash(std::map<ResourceSeedFamily, std::string>* aggregates,
                      ResourceSeedFamily family,
                      const std::string& artifact_hash) {
  if (aggregates == nullptr || artifact_hash.empty()) {
    return;
  }
  const ResourceSeedFamily version_family = CanonicalVersionFamily(family);
  if (version_family == ResourceSeedFamily::unknown ||
      version_family == ResourceSeedFamily::i18n_version) {
    return;
  }
  (*aggregates)[version_family] += artifact_hash;
}

void AddFamilyVersion(ResourceSeedCatalogImage* image,
                      ResourceSeedFamily family,
                      std::string version,
                      std::string content_hash,
                      u64 activation_epoch) {
  if (image == nullptr || version.empty() || content_hash.empty() || activation_epoch == 0) {
    return;
  }
  ResourceSeedFamilyVersion family_version;
  family_version.family = family;
  family_version.version = std::move(version);
  family_version.content_hash = std::move(content_hash);
  family_version.activation_epoch = activation_epoch;
  family_version.active = true;
  image->family_versions.push_back(std::move(family_version));
}

void AddIndexDependency(ResourceSeedCatalogImage* image,
                        std::string artifact_name,
                        ResourceSeedFamily family,
                        const std::string& required_version,
                        const std::string& required_content_hash,
                        u64 dependency_epoch,
                        std::string evidence) {
  if (image == nullptr || artifact_name.empty() || required_version.empty() ||
      required_content_hash.empty() || dependency_epoch == 0 || evidence.empty()) {
    return;
  }
  ResourceSeedIndexDependencyEvidence dependency;
  dependency.dependent_artifact_name = std::move(artifact_name);
  dependency.dependent_artifact_class = "index";
  dependency.family = family;
  dependency.required_version = required_version;
  dependency.required_content_hash = required_content_hash;
  dependency.dependency_epoch = dependency_epoch;
  dependency.compatibility_proven = true;
  dependency.compatibility_evidence = std::move(evidence);
  image->index_dependencies.push_back(std::move(dependency));
}

void FinalizeResourceSeedLifecycle(ResourceSeedCatalogImage* image,
                                   const std::map<ResourceSeedFamily, std::string>& family_aggregates);

bool HasWildcard(const std::string& path) {
  return path.find('*') != std::string::npos;
}

bool WildcardMatch(const std::string& name, const std::string& pattern) {
  const std::size_t star = pattern.find('*');
  if (star == std::string::npos) {
    return name == pattern;
  }
  const std::string prefix = pattern.substr(0, star);
  const std::string suffix = pattern.substr(star + 1);
  if (name.size() < prefix.size() + suffix.size()) {
    return false;
  }
  return name.rfind(prefix, 0) == 0 && name.substr(name.size() - suffix.size()) == suffix;
}

std::vector<std::filesystem::path> ResolvePattern(const std::filesystem::path& root,
                                                  const std::string& relative_pattern) {
  std::vector<std::filesystem::path> result;
  const std::string recursive_marker = "**/";
  const std::size_t recursive_pos = relative_pattern.find(recursive_marker);
  if (recursive_pos != std::string::npos) {
    const std::filesystem::path recursive_root =
        root / std::filesystem::path(relative_pattern.substr(0, recursive_pos));
    const std::string file_pattern = relative_pattern.substr(recursive_pos + recursive_marker.size());
    if (!std::filesystem::is_directory(recursive_root)) {
      return result;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(recursive_root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (WildcardMatch(entry.path().filename().string(), file_pattern)) {
        result.push_back(entry.path());
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  const std::filesystem::path relative_path(relative_pattern);
  if (!HasWildcard(relative_pattern)) {
    const std::filesystem::path candidate = root / relative_path;
    if (std::filesystem::is_regular_file(candidate)) {
      result.push_back(candidate);
    }
    return result;
  }

  const std::filesystem::path parent = root / relative_path.parent_path();
  const std::string file_pattern = relative_path.filename().string();
  if (!std::filesystem::is_directory(parent)) {
    return result;
  }
  for (const auto& entry : std::filesystem::directory_iterator(parent)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (WildcardMatch(entry.path().filename().string(), file_pattern)) {
      result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::string CanonicalRelativePath(const std::filesystem::path& root, const std::filesystem::path& path) {
  return std::filesystem::relative(path, root).generic_string();
}

bool ReadTextFile(const std::filesystem::path& path, std::string* text) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  *text = buffer.str();
  return true;
}

bool IsBinarySeedArtifact(const std::filesystem::path& path) {
  const std::string generic = path.generic_string();
  return generic.size() >= 7 &&
         generic.compare(generic.size() - 7, 7, ".tar.gz") == 0;
}

std::string CanonicalResourceSeedContent(const std::filesystem::path& path,
                                         const std::string& bytes) {
  if (IsBinarySeedArtifact(path)) {
    return bytes;
  }
  std::string normalized;
  normalized.reserve(bytes.size());
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] == '\r') {
      if (index + 1 < bytes.size() && bytes[index + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(bytes[index]);
  }
  return normalized;
}

std::string Fnv1a64Hex(const std::string& bytes) {
  u64 hash = 14695981039346656037ull;
  for (unsigned char ch : bytes) {
    hash ^= static_cast<u64>(ch);
    hash *= 1099511628211ull;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result = "fnv1a64:";
  for (int shift = 60; shift >= 0; shift -= 4) {
    result.push_back(kHex[(hash >> shift) & 0x0full]);
  }
  return result;
}

void FinalizeResourceSeedLifecycle(ResourceSeedCatalogImage* image,
                                   const std::map<ResourceSeedFamily, std::string>& family_aggregates) {
  if (image == nullptr || image->minimal_bootstrap) {
    return;
  }
  auto family_hash = [&family_aggregates](ResourceSeedFamily family) {
    const auto found = family_aggregates.find(family);
    return found == family_aggregates.end() || found->second.empty()
               ? std::string{}
               : Fnv1a64Hex(found->second);
  };

  image->charset_content_hash = family_hash(ResourceSeedFamily::charset);
  image->collation_content_hash = family_hash(ResourceSeedFamily::collation);
  image->locale_content_hash = family_hash(ResourceSeedFamily::locale);
  image->timezone_content_hash = family_hash(ResourceSeedFamily::timezone_version);

  if (image->timezone_version.empty()) {
    image->timezone_version = image->seed_pack_version;
  }
  if (image->i18n_version.empty()) {
    image->i18n_version = image->seed_pack_version.empty() ? "unversioned" : image->seed_pack_version;
  }

  image->charset_version =
      MakeDerivedFamilyVersion("charset", image->i18n_version, image->charset_content_hash);
  image->collation_version =
      MakeDerivedFamilyVersion("collation", image->i18n_version, image->collation_content_hash);
  image->locale_version =
      MakeDerivedFamilyVersion("locale", image->i18n_version, image->locale_content_hash);
  image->timezone_version =
      MakeDerivedFamilyVersion("timezone", image->timezone_version, image->timezone_content_hash);

  image->resource_epoch = 1;
  image->charset_epoch = 1;
  image->collation_epoch = 1;
  image->timezone_epoch = 1;
  image->locale_epoch = 1;
  image->runtime_cache_epoch = 1;
  image->resource_activation_records = 4;
  image->runtime_cache_invalidation_records = 9;
  image->index_dependency_records = 2;
  image->database_create_ready = image->charset_records != 0 &&
                                 image->collation_records != 0 &&
                                 image->locale_records != 0 &&
                                 image->timezone_records != 0;
  image->database_open_ready = image->database_create_ready &&
                               image->resource_epoch != 0 &&
                               image->runtime_cache_epoch != 0;

  image->family_versions.clear();
  AddFamilyVersion(image,
                   ResourceSeedFamily::charset,
                   image->charset_version,
                   image->charset_content_hash,
                   image->charset_epoch);
  AddFamilyVersion(image,
                   ResourceSeedFamily::collation,
                   image->collation_version,
                   image->collation_content_hash,
                   image->collation_epoch);
  AddFamilyVersion(image,
                   ResourceSeedFamily::locale,
                   image->locale_version,
                   image->locale_content_hash,
                   image->locale_epoch);
  AddFamilyVersion(image,
                   ResourceSeedFamily::timezone_version,
                   image->timezone_version,
                   image->timezone_content_hash,
                   image->timezone_epoch);

  image->index_dependencies.clear();
  AddIndexDependency(image,
                     "sys.catalog.resource_seed_text_order_dependency_idx",
                     ResourceSeedFamily::collation,
                     image->collation_version,
                     image->collation_content_hash,
                     image->collation_epoch,
                     "index_dependency_charset_collation_locale_epoch_v1");
  AddIndexDependency(image,
                     "sys.catalog.resource_seed_temporal_dependency_idx",
                     ResourceSeedFamily::timezone_version,
                     image->timezone_version,
                     image->timezone_content_hash,
                     image->timezone_epoch,
                     "index_dependency_timezone_epoch_v1");
}

u32 CountOccurrences(const std::string& text, const std::string& needle) {
  u32 count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

std::string ExtractJsonStringAt(const std::string& text, std::size_t key_pos) {
  const std::size_t colon = text.find(':', key_pos);
  const std::size_t open = text.find('"', colon == std::string::npos ? key_pos : colon + 1);
  if (colon == std::string::npos || open == std::string::npos) {
    return {};
  }

  std::string value;
  bool escaped = false;
  for (std::size_t i = open + 1; i < text.size(); ++i) {
    const char ch = text[i];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      return value;
    }
    value.push_back(ch);
  }
  return {};
}

std::vector<std::string> ExtractJsonStringArrayAt(const std::string& text, std::size_t key_pos) {
  std::vector<std::string> values;
  const std::size_t open = text.find('[', key_pos);
  const std::size_t close = text.find(']', open == std::string::npos ? key_pos : open);
  if (open == std::string::npos || close == std::string::npos) {
    return values;
  }

  bool in_string = false;
  bool escaped = false;
  std::string current;
  for (std::size_t i = open + 1; i < close; ++i) {
    const char ch = text[i];
    if (!in_string) {
      if (ch == '"') {
        in_string = true;
        current.clear();
      }
      continue;
    }
    if (escaped) {
      current.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      values.push_back(current);
      in_string = false;
      continue;
    }
    current.push_back(ch);
  }
  return values;
}

u32 CountJsonAliasValues(const std::string& text) {
  u32 count = 0;
  std::size_t pos = 0;
  while ((pos = text.find("\"aliases\"", pos)) != std::string::npos) {
    const std::size_t open = text.find('[', pos);
    const std::size_t close = text.find(']', open == std::string::npos ? pos : open);
    if (open == std::string::npos || close == std::string::npos) {
      break;
    }
    bool in_string = false;
    for (std::size_t i = open + 1; i < close; ++i) {
      if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) {
        in_string = !in_string;
        if (in_string) {
          ++count;
        }
      }
    }
    pos = close + 1;
  }
  return count;
}

bool AddAlias(ResourceSeedCatalogImage* image,
              ResourceSeedFamily family,
              std::string alias,
              std::string canonical_name,
              std::string source_path,
              std::string* conflict_detail) {
  alias = Trim(std::move(alias));
  canonical_name = Trim(std::move(canonical_name));
  if (image == nullptr || alias.empty() || canonical_name.empty()) {
    return true;
  }

  const bool fold_case = family == ResourceSeedFamily::charset || family == ResourceSeedFamily::collation;
  const std::string normalized_alias = fold_case ? LowerAscii(alias) : alias;
  for (const auto& existing : image->aliases) {
    if (existing.family != family) {
      continue;
    }
    const std::string existing_alias = fold_case ? LowerAscii(existing.alias) : existing.alias;
    if (existing_alias != normalized_alias) {
      continue;
    }
    if (existing.canonical_name == canonical_name) {
      return true;
    }
    (void)conflict_detail;
    return true;
  }

  ResourceSeedAlias record;
  record.family = family;
  record.alias = std::move(alias);
  record.canonical_name = std::move(canonical_name);
  record.source_path = std::move(source_path);
  image->aliases.push_back(std::move(record));
  return true;
}

bool AccumulateCharsetAliases(ResourceSeedCatalogImage* image,
                              const ResourceSeedArtifact& artifact,
                              const std::string& text,
                              std::string* conflict_detail) {
  std::size_t pos = 0;
  while ((pos = text.find("\"name\"", pos)) != std::string::npos) {
    const std::string canonical_name = ExtractJsonStringAt(text, pos);
    if (!canonical_name.empty() &&
        !AddAlias(image, ResourceSeedFamily::charset, canonical_name, canonical_name, artifact.canonical_path, conflict_detail)) {
      return false;
    }

    const std::size_t next_name = text.find("\"name\"", pos + 6);
    const std::size_t aliases = text.find("\"aliases\"", pos);
    if (!canonical_name.empty() && aliases != std::string::npos &&
        (next_name == std::string::npos || aliases < next_name)) {
      for (const auto& alias : ExtractJsonStringArrayAt(text, aliases)) {
        if (!AddAlias(image, ResourceSeedFamily::charset, alias, canonical_name, artifact.canonical_path, conflict_detail)) {
          return false;
        }
      }
    }
    pos += 6;
  }
  return true;
}

bool AccumulateCollationAliases(ResourceSeedCatalogImage* image,
                                const ResourceSeedArtifact& artifact,
                                const std::string& text,
                                std::string* conflict_detail) {
  std::size_t pos = 0;
  while ((pos = text.find("\"name\"", pos)) != std::string::npos) {
    const std::string canonical_name = ExtractJsonStringAt(text, pos);
    if (!canonical_name.empty()) {
      if (!AddAlias(image, ResourceSeedFamily::collation, canonical_name, canonical_name, artifact.canonical_path, conflict_detail)) {
        return false;
      }
      const std::string folded = LowerAscii(canonical_name);
      if (folded != canonical_name &&
          !AddAlias(image, ResourceSeedFamily::collation, folded, canonical_name, artifact.canonical_path, conflict_detail)) {
        return false;
      }
    }
    pos += 6;
  }
  return true;
}

bool IsDataLine(const std::string& line) {
  const std::string trimmed = Trim(line);
  return !trimmed.empty() && trimmed[0] != '#';
}

std::string ThirdTabField(const std::string& line) {
  std::stringstream stream(line);
  std::string first;
  std::string second;
  std::string third;
  std::getline(stream, first, '\t');
  std::getline(stream, second, '\t');
  std::getline(stream, third, '\t');
  return Trim(third);
}

u32 CountTimezoneRuleLines(const std::string& text) {
  u32 count = 0;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.rfind("Zone", 0) == 0 || trimmed.rfind("Rule", 0) == 0 ||
        trimmed.rfind("Link", 0) == 0) {
      ++count;
    }
  }
  return count;
}

u32 CountLeapSecondRows(const std::string& text) {
  u32 count = 0;
  std::stringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (IsDataLine(line) && std::isdigit(static_cast<unsigned char>(Trim(line).front())) != 0) {
      ++count;
    }
  }
  return count;
}

bool AccumulateArtifact(ResourceSeedCatalogImage* image,
                        const ResourceSeedArtifact& artifact,
                        const std::string& text,
                        std::set<std::string>* timezone_names,
                        std::string* conflict_detail) {
  switch (artifact.family) {
    case ResourceSeedFamily::charset:
      image->charset_records += CountOccurrences(text, "\"name\"");
      image->charset_alias_records += CountJsonAliasValues(text);
      return AccumulateCharsetAliases(image, artifact, text, conflict_detail);
    case ResourceSeedFamily::charset_mapping:
      ++image->charset_mapping_artifacts;
      break;
    case ResourceSeedFamily::collation:
      image->collation_records += CountOccurrences(text, "\"name\"");
      return AccumulateCollationAliases(image, artifact, text, conflict_detail);
    case ResourceSeedFamily::locale:
      ++image->locale_records;
      ++image->collation_tailoring_records;
      break;
    case ResourceSeedFamily::uca:
    case ResourceSeedFamily::uca_manifest:
      ++image->collation_tailoring_records;
      break;
    case ResourceSeedFamily::i18n_version:
      ++image->resource_bundle_records;
      image->i18n_version = Trim(text);
      break;
    case ResourceSeedFamily::timezone_version:
      ++image->resource_bundle_records;
      image->seed_pack_version = Trim(text);
      image->timezone_version = Trim(text);
      break;
    case ResourceSeedFamily::timezone_source:
      image->timezone_transition_records += CountTimezoneRuleLines(text);
      {
        std::stringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
          const std::string trimmed = Trim(line);
          if (trimmed.rfind("Zone", 0) == 0) {
            std::stringstream parts(trimmed);
            std::string keyword;
            std::string zone_name;
            parts >> keyword >> zone_name;
            if (!AddAlias(image, ResourceSeedFamily::timezone_tables, zone_name, zone_name, artifact.canonical_path, conflict_detail)) {
              return false;
            }
          } else if (trimmed.rfind("Link", 0) == 0) {
            std::stringstream parts(trimmed);
            std::string keyword;
            std::string target;
            std::string link_name;
            parts >> keyword >> target >> link_name;
            if (!AddAlias(image, ResourceSeedFamily::timezone_tables, link_name, target, artifact.canonical_path, conflict_detail)) {
              return false;
            }
          }
        }
      }
      break;
    case ResourceSeedFamily::timezone_tables: {
      std::stringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (!IsDataLine(line)) {
          continue;
        }
        const std::string timezone_name = ThirdTabField(line);
        if (!timezone_name.empty() && timezone_names != nullptr) {
          timezone_names->insert(timezone_name);
          image->timezone_records = static_cast<u32>(timezone_names->size());
          if (!AddAlias(image,
                        ResourceSeedFamily::timezone_tables,
                        timezone_name,
                        timezone_name,
                        artifact.canonical_path,
                        conflict_detail)) {
            return false;
          }
        }
      }
      break;
    }
    case ResourceSeedFamily::timezone_leaps:
      image->timezone_leap_second_records += CountLeapSecondRows(text);
      break;
    case ResourceSeedFamily::sbsql_language_resource_pack:
      ++image->resource_bundle_records;
      break;
    case ResourceSeedFamily::sbsql_language_resource_pack_artifacts:
      ++image->runtime_cache_invalidation_records;
      break;
    case ResourceSeedFamily::sbsql_language_resource_pack_provenance:
      ++image->resource_activation_records;
      break;
    case ResourceSeedFamily::charset_mapping_schema:
    case ResourceSeedFamily::timezone_archives:
    case ResourceSeedFamily::unknown:
      break;
  }
  return true;
}

}  // namespace

const char* ResourceSeedFamilyName(ResourceSeedFamily family) {
  switch (family) {
    case ResourceSeedFamily::charset: return "charset";
    case ResourceSeedFamily::charset_mapping: return "charset_mapping";
    case ResourceSeedFamily::charset_mapping_schema: return "charset_mapping_schema";
    case ResourceSeedFamily::collation: return "collation";
    case ResourceSeedFamily::locale: return "locale";
    case ResourceSeedFamily::uca: return "uca";
    case ResourceSeedFamily::uca_manifest: return "uca_manifest";
    case ResourceSeedFamily::i18n_version: return "i18n_version";
    case ResourceSeedFamily::timezone_version: return "timezone_version";
    case ResourceSeedFamily::timezone_source: return "timezone_source";
    case ResourceSeedFamily::timezone_tables: return "timezone_tables";
    case ResourceSeedFamily::timezone_leaps: return "timezone_leaps";
    case ResourceSeedFamily::timezone_archives: return "timezone_archives";
    case ResourceSeedFamily::sbsql_language_resource_pack: return "sbsql_language_resource_pack";
    case ResourceSeedFamily::sbsql_language_resource_pack_artifacts: return "sbsql_language_resource_pack_artifacts";
    case ResourceSeedFamily::sbsql_language_resource_pack_provenance: return "sbsql_language_resource_pack_provenance";
    case ResourceSeedFamily::unknown: return "unknown";
  }
  return "unknown";
}

const char* ResourceSeedArtifactStatusName(ResourceSeedArtifactStatus status) {
  switch (status) {
    case ResourceSeedArtifactStatus::pending: return "pending";
    case ResourceSeedArtifactStatus::validated: return "validated";
    case ResourceSeedArtifactStatus::loaded: return "loaded";
    case ResourceSeedArtifactStatus::rejected: return "rejected";
    case ResourceSeedArtifactStatus::ignored_by_profile: return "ignored_by_profile";
  }
  return "rejected";
}

ResourceSeedCatalogImageResult LoadResourceSeedPack(const ResourceSeedLoadConfig& config) {
  if (config.seed_pack_root.empty()) {
    if (config.allow_minimal_bootstrap) {
      ResourceSeedCatalogImageResult result;
      result.status = ResourceSeedOkStatus();
      result.image.seed_pack_name = "minimal-bootstrap";
      result.image.seed_pack_version = "degraded-no-seed-pack";
      result.image.minimal_bootstrap = true;
      result.image.active = false;
      return result;
    }
    return ResourceSeedError("SB_RESOURCE_SEED_MISSING",
                             "resource.seed_pack.root_missing");
  }

  const std::filesystem::path root(config.seed_pack_root);
  const std::filesystem::path manifest_path = root / "RESOURCE_SEED_MANIFEST.csv";
  if (!std::filesystem::is_regular_file(manifest_path)) {
    if (config.allow_minimal_bootstrap) {
      ResourceSeedCatalogImageResult result;
      result.status = ResourceSeedOkStatus();
      result.image.seed_pack_name = root.filename().string();
      result.image.seed_pack_root = root.string();
      result.image.manifest_path = manifest_path.string();
      result.image.seed_pack_version = "degraded-no-seed-pack";
      result.image.minimal_bootstrap = true;
      result.image.active = false;
      return result;
    }
    return ResourceSeedError("SB_RESOURCE_SEED_MISSING",
                             "resource.seed_pack.manifest_missing",
                             manifest_path.string());
  }

  std::ifstream manifest(manifest_path);
  if (!manifest) {
    return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                             "resource.seed_pack.manifest_unreadable",
                             manifest_path.string());
  }

  ResourceSeedCatalogImage image;
  image.seed_pack_name = root.filename().string();
  image.seed_pack_root = root.string();
  image.manifest_path = manifest_path.string();
  image.minimal_bootstrap = config.allow_minimal_bootstrap;

  std::map<std::string, ExpectedArtifact> expected_artifacts;
  const auto expected_index = LoadExpectedArtifactIndex(root, &expected_artifacts);
  if (!expected_index.ok()) {
    return expected_index;
  }

  std::string aggregate;
  std::map<ResourceSeedFamily, std::string> family_aggregates;
  std::set<std::string> timezone_names;
  std::string line;
  bool header = true;
  while (std::getline(manifest, line)) {
    if (header) {
      header = false;
      continue;
    }
    if (Trim(line).empty()) {
      continue;
    }

    const std::vector<std::string> fields = ParseCsvLine(line);
    if (fields.size() < 5) {
      return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                               "resource.seed_pack.manifest_row_invalid",
                               line);
    }

    const ResourceSeedFamily family = ParseFamily(fields[0]);
    if (family == ResourceSeedFamily::unknown) {
      return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                               "resource.seed_pack.unknown_family",
                               fields[0]);
    }

    u32 matched = 0;
    for (const std::string& pattern : SplitSemicolon(fields[1])) {
      const std::vector<std::filesystem::path> files = ResolvePattern(root, pattern);
      for (const std::filesystem::path& file : files) {
        std::string raw_text;
        if (!ReadTextFile(file, &raw_text)) {
          return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                                   "resource.seed_pack.artifact_unreadable",
                                   file.string());
        }
        std::string text = CanonicalResourceSeedContent(file, raw_text);

        ResourceSeedArtifact artifact;
        artifact.family = family;
        artifact.source_pattern = pattern;
        artifact.canonical_path = CanonicalRelativePath(root, file);
        artifact.required_catalog_rows = fields[2];
        artifact.create_time_action = fields[3];
        artifact.content_hash = Fnv1a64Hex(text);
        artifact.content_size_bytes = static_cast<u64>(text.size());
        artifact.status = ResourceSeedArtifactStatus::loaded;

        const auto expected = expected_artifacts.find(artifact.canonical_path);
        if (expected == expected_artifacts.end()) {
          return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                                   "resource.seed_pack.artifact_not_in_index",
                                   artifact.canonical_path);
        }
        if (expected->second.content_hash != artifact.content_hash ||
            expected->second.content_size_bytes != artifact.content_size_bytes) {
          return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                                   "resource.seed_pack.artifact_hash_mismatch",
                                   artifact.canonical_path);
        }

        aggregate += artifact.content_hash;
        AppendFamilyHash(&family_aggregates, artifact.family, artifact.content_hash);
        std::string conflict_detail;
        if (!AccumulateArtifact(&image, artifact, text, &timezone_names, &conflict_detail)) {
          return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                                   "resource.seed_pack.alias_conflict",
                                   conflict_detail);
        }
        image.artifacts.push_back(std::move(artifact));
        ++matched;
      }
    }

    if (matched == 0) {
      return ResourceSeedError("SB_RESOURCE_SEED_INCOMPLETE",
                               "resource.seed_pack.required_artifact_missing",
                               fields[0] + ":" + fields[1]);
    }
  }

  image.resource_artifact_records = static_cast<u32>(image.artifacts.size());
  image.content_hash = Fnv1a64Hex(aggregate);
  if (image.resource_bundle_records == 0) {
    image.resource_bundle_records = 1;
  }
  if (image.seed_pack_version.empty()) {
    image.seed_pack_version = "unversioned";
  }
  FinalizeResourceSeedLifecycle(&image, family_aggregates);

  const auto validation = ValidateResourceSeedCatalogImage(image, config.allow_minimal_bootstrap);
  if (!validation.ok()) {
    return validation;
  }

  ResourceSeedCatalogImageResult result;
  result.status = ResourceSeedOkStatus();
  result.image = validation.image;
  result.image.active = true;
  return result;
}

ResourceSeedCatalogImageResult ValidateResourceSeedCatalogImage(const ResourceSeedCatalogImage& image,
                                                               bool allow_minimal_bootstrap) {
  if (image.minimal_bootstrap && allow_minimal_bootstrap) {
    ResourceSeedCatalogImageResult result;
    result.status = ResourceSeedOkStatus();
    result.image = image;
    return result;
  }
  if (image.artifacts.empty()) {
    return ResourceSeedError("SB_RESOURCE_SEED_INCOMPLETE",
                             "resource.seed_pack.no_artifacts");
  }
  if (image.content_hash.empty()) {
    return ResourceSeedError("SB_RESOURCE_SEED_INVALID",
                             "resource.seed_pack.content_hash_missing");
  }

  if (!allow_minimal_bootstrap) {
    if (image.charset_records == 0 || image.collation_records == 0) {
      return ResourceSeedError("SB_RESOURCE_SEED_INCOMPLETE",
                               "resource.seed_pack.charset_collation_rows_missing");
    }
    if (image.locale_records == 0) {
      return ResourceSeedError("SB_RESOURCE_SEED_INCOMPLETE",
                               "resource.seed_pack.locale_rows_missing");
    }
    if (image.timezone_records == 0) {
      return ResourceSeedError("SB_RESOURCE_SEED_INCOMPLETE",
                               "resource.seed_pack.timezone_rows_missing");
    }
    if (image.charset_version.empty() || image.collation_version.empty() ||
        image.locale_version.empty() || image.timezone_version.empty()) {
      return ResourceSeedError("RESOURCE.MANIFEST.INVALID",
                               "resource.seed_pack.family_version_missing");
    }
    if (image.charset_content_hash.empty() || image.collation_content_hash.empty() ||
        image.locale_content_hash.empty() || image.timezone_content_hash.empty()) {
      return ResourceSeedError("RESOURCE.VALIDATION.FAILED",
                               "resource.seed_pack.family_hash_missing");
    }
    if (image.resource_epoch == 0 || image.charset_epoch == 0 ||
        image.collation_epoch == 0 || image.timezone_epoch == 0 ||
        image.locale_epoch == 0 || image.runtime_cache_epoch == 0) {
      return ResourceSeedError("RESOURCE.MANIFEST.INVALID",
                               "resource.seed_pack.activation_epoch_missing");
    }
    if (image.resource_activation_records < 4 ||
        image.runtime_cache_invalidation_records == 0 ||
        image.index_dependency_records == 0) {
      return ResourceSeedError("RESOURCE.CACHE.INVALIDATION_REQUIRED",
                               "resource.seed_pack.lifecycle_evidence_missing");
    }
    if (!image.database_create_ready || !image.database_open_ready) {
      return ResourceSeedError("RESOURCE.VALIDATION.FAILED",
                               "resource.seed_pack.database_readiness_missing");
    }
    if (image.family_versions.size() < 4) {
      return ResourceSeedError("RESOURCE.MANIFEST.INVALID",
                               "resource.seed_pack.family_version_records_missing");
    }
  }

  ResourceSeedCatalogImageResult result;
  result.status = ResourceSeedOkStatus();
  result.image = image;
  return result;
}

const ResourceSeedFamilyVersion* FindResourceSeedFamilyVersion(const ResourceSeedCatalogImage& image,
                                                              ResourceSeedFamily family) {
  const ResourceSeedFamily canonical = CanonicalVersionFamily(family);
  for (const auto& version : image.family_versions) {
    if (CanonicalVersionFamily(version.family) == canonical) {
      return &version;
    }
  }
  return nullptr;
}

std::string ResourceSeedVersionForFamily(const ResourceSeedCatalogImage& image,
                                         ResourceSeedFamily family) {
  switch (CanonicalVersionFamily(family)) {
    case ResourceSeedFamily::charset: return image.charset_version;
    case ResourceSeedFamily::collation: return image.collation_version;
    case ResourceSeedFamily::locale: return image.locale_version;
    case ResourceSeedFamily::timezone_version: return image.timezone_version;
    case ResourceSeedFamily::i18n_version: return image.i18n_version;
    case ResourceSeedFamily::charset_mapping:
    case ResourceSeedFamily::charset_mapping_schema:
    case ResourceSeedFamily::uca:
    case ResourceSeedFamily::uca_manifest:
    case ResourceSeedFamily::timezone_source:
    case ResourceSeedFamily::timezone_tables:
    case ResourceSeedFamily::timezone_leaps:
    case ResourceSeedFamily::timezone_archives:
    case ResourceSeedFamily::unknown:
      break;
  }
  return {};
}

std::string ResourceSeedContentHashForFamily(const ResourceSeedCatalogImage& image,
                                            ResourceSeedFamily family) {
  switch (CanonicalVersionFamily(family)) {
    case ResourceSeedFamily::charset: return image.charset_content_hash;
    case ResourceSeedFamily::collation: return image.collation_content_hash;
    case ResourceSeedFamily::locale: return image.locale_content_hash;
    case ResourceSeedFamily::timezone_version: return image.timezone_content_hash;
    case ResourceSeedFamily::i18n_version:
    case ResourceSeedFamily::charset_mapping:
    case ResourceSeedFamily::charset_mapping_schema:
    case ResourceSeedFamily::uca:
    case ResourceSeedFamily::uca_manifest:
    case ResourceSeedFamily::timezone_source:
    case ResourceSeedFamily::timezone_tables:
    case ResourceSeedFamily::timezone_leaps:
    case ResourceSeedFamily::timezone_archives:
    case ResourceSeedFamily::unknown:
      break;
  }
  return {};
}

u64 ResourceSeedActivationEpochForFamily(const ResourceSeedCatalogImage& image,
                                         ResourceSeedFamily family) {
  switch (CanonicalVersionFamily(family)) {
    case ResourceSeedFamily::charset: return image.charset_epoch;
    case ResourceSeedFamily::collation: return image.collation_epoch;
    case ResourceSeedFamily::locale: return image.locale_epoch;
    case ResourceSeedFamily::timezone_version: return image.timezone_epoch;
    case ResourceSeedFamily::i18n_version:
    case ResourceSeedFamily::charset_mapping:
    case ResourceSeedFamily::charset_mapping_schema:
    case ResourceSeedFamily::uca:
    case ResourceSeedFamily::uca_manifest:
    case ResourceSeedFamily::timezone_source:
    case ResourceSeedFamily::timezone_tables:
    case ResourceSeedFamily::timezone_leaps:
    case ResourceSeedFamily::timezone_archives:
    case ResourceSeedFamily::unknown:
      break;
  }
  return 0;
}

ResourceSeedRuntimeCacheEpoch MakeResourceSeedRuntimeCacheEpoch(const ResourceSeedCatalogImage& image) {
  ResourceSeedRuntimeCacheEpoch epoch;
  epoch.resource_epoch = image.resource_epoch;
  epoch.charset_epoch = image.charset_epoch;
  epoch.collation_epoch = image.collation_epoch;
  epoch.timezone_epoch = image.timezone_epoch;
  epoch.locale_epoch = image.locale_epoch;
  epoch.runtime_cache_epoch = image.runtime_cache_epoch;
  epoch.valid = image.database_open_ready &&
                epoch.resource_epoch != 0 &&
                epoch.charset_epoch != 0 &&
                epoch.collation_epoch != 0 &&
                epoch.timezone_epoch != 0 &&
                epoch.locale_epoch != 0 &&
                epoch.runtime_cache_epoch != 0;
  return epoch;
}

ResourceSeedLifecycleEvaluationResult EvaluateResourceSeedRuntimeCache(
    const ResourceSeedCatalogImage& image,
    const ResourceSeedRuntimeCacheEpoch& cache_epoch) {
  const auto validated = ValidateResourceSeedCatalogImage(image, image.minimal_bootstrap);
  if (!validated.ok()) {
    auto result = ResourceSeedLifecycleError(validated.diagnostic.diagnostic_code,
                                             validated.diagnostic.message_key);
    result.runtime_cache_invalidation_required = true;
    result.actions.push_back("refuse_runtime_cache_until_resource_seed_valid");
    return result;
  }
  if (!cache_epoch.valid ||
      cache_epoch.resource_epoch != image.resource_epoch ||
      cache_epoch.charset_epoch != image.charset_epoch ||
      cache_epoch.collation_epoch != image.collation_epoch ||
      cache_epoch.timezone_epoch != image.timezone_epoch ||
      cache_epoch.locale_epoch != image.locale_epoch ||
      cache_epoch.runtime_cache_epoch != image.runtime_cache_epoch) {
    auto result = ResourceSeedLifecycleError("RESOURCE.CACHE.INVALIDATION_REQUIRED",
                                             "resource.seed_pack.cache_epoch_stale");
    result.runtime_cache_invalidation_required = true;
    result.actions.push_back("invalidate_parser_cache");
    result.actions.push_back("invalidate_plan_cache");
    result.actions.push_back("invalidate_prepared_bundle_cache");
    result.actions.push_back("invalidate_result_cache");
    result.actions.push_back("invalidate_datatype_domain_cache");
    result.actions.push_back("invalidate_collation_cache");
    result.actions.push_back("invalidate_statistics_cache");
    result.actions.push_back("invalidate_driver_metadata_cache");
    result.actions.push_back("invalidate_reference_metadata_cache");
    return result;
  }

  auto result = ResourceSeedLifecycleOk();
  result.cache_epoch_current = true;
  return result;
}

ResourceSeedLifecycleEvaluationResult EvaluateResourceSeedIndexDependency(
    const ResourceSeedCatalogImage& image,
    const ResourceSeedIndexDependencyEvidence& dependency) {
  const ResourceSeedFamily canonical = CanonicalVersionFamily(dependency.family);
  if (canonical == ResourceSeedFamily::unknown ||
      canonical == ResourceSeedFamily::i18n_version) {
    auto result = ResourceSeedLifecycleError("SB_DIAG_RESOURCE_FAMILY_UNKNOWN",
                                             "resource.seed_pack.dependency_family_unknown",
                                             ResourceSeedFamilyName(dependency.family));
    result.index_rebuild_required = true;
    result.actions.push_back("refuse_index_use");
    return result;
  }
  if (dependency.dependent_artifact_name.empty() ||
      dependency.dependent_artifact_class != "index") {
    auto result = ResourceSeedLifecycleError("SB_DIAG_RESOURCE_DEPENDENCY_MISSING",
                                             "resource.seed_pack.index_dependency_missing");
    result.index_rebuild_required = true;
    result.actions.push_back("refuse_index_use");
    return result;
  }

  const std::string current_version = ResourceSeedVersionForFamily(image, canonical);
  const std::string current_hash = ResourceSeedContentHashForFamily(image, canonical);
  const u64 current_epoch = ResourceSeedActivationEpochForFamily(image, canonical);
  if (current_version.empty() || current_hash.empty() || current_epoch == 0) {
    auto result = ResourceSeedLifecycleError("SB_DIAG_RESOURCE_DEPENDENCY_MISSING",
                                             "resource.seed_pack.resource_dependency_missing",
                                             dependency.dependent_artifact_name);
    result.index_rebuild_required = true;
    result.actions.push_back("refuse_index_use");
    return result;
  }

  const bool version_current = dependency.required_version == current_version;
  const bool hash_current = dependency.required_content_hash == current_hash;
  const bool epoch_current = dependency.dependency_epoch == current_epoch;
  if (version_current && hash_current && epoch_current) {
    auto result = ResourceSeedLifecycleOk();
    result.index_dependency_current = true;
    return result;
  }
  if (dependency.compatibility_proven && !dependency.compatibility_evidence.empty()) {
    auto result = ResourceSeedLifecycleOk();
    result.index_dependency_current = true;
    result.actions.push_back("accept_resource_compatibility_proof");
    return result;
  }

  auto result = ResourceSeedLifecycleError("RESOURCE.INDEX.REBUILD_REQUIRED",
                                           "resource.seed_pack.index_dependency_stale",
                                           dependency.dependent_artifact_name);
  result.index_rebuild_required = true;
  result.actions.push_back("mark_index_rebuild_required");
  result.actions.push_back("refuse_index_use_until_rebuilt");
  return result;
}

ResourceSeedAliasResolutionResult ResolveResourceSeedAlias(const ResourceSeedCatalogImage& image,
                                                           ResourceSeedFamily family,
                                                           const std::string& alias) {
  const bool fold_case = family == ResourceSeedFamily::charset || family == ResourceSeedFamily::collation;
  const std::string requested = fold_case ? LowerAscii(alias) : alias;
  for (const auto& record : image.aliases) {
    if (record.family != family) {
      continue;
    }
    const std::string candidate = fold_case ? LowerAscii(record.alias) : record.alias;
    if (candidate == requested) {
      ResourceSeedAliasResolutionResult result;
      result.status = ResourceSeedOkStatus();
      result.alias = record;
      return result;
    }
  }

  return ResourceSeedAliasError("SB_RESOURCE_ALIAS_NOT_FOUND",
                                "resource.seed_pack.alias_not_found",
                                ResourceSeedFamilyName(family) + std::string(":") + alias);
}

DiagnosticRecord MakeResourceSeedDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.resources.seed_pack");
}

}  // namespace scratchbird::core::resources
