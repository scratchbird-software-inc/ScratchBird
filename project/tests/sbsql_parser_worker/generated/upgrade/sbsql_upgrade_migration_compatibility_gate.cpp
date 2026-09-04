// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/function_seed_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;

struct CsvTable {
  std::filesystem::path path;
  std::vector<std::string> headers;
  std::vector<CsvRow> rows;
};

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string message) {
    if (condition) return;
    ok = false;
    if (failures < 120) std::cerr << message << '\n';
    ++failures;
  }
};

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

void TrimLineEnding(std::string* line) {
  while (!line->empty() && (line->back() == '\r' || line->back() == '\n')) {
    line->pop_back();
  }
}

std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
      continue;
    }

    if (ch == '"') {
      in_quotes = true;
    } else if (ch == ',') {
      fields.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }

  fields.push_back(current);
  return fields;
}

CsvTable ReadCsv(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());

  std::string line;
  if (!std::getline(input, line)) throw std::runtime_error("empty CSV " + path.string());
  TrimLineEnding(&line);

  CsvTable table;
  table.path = path;
  table.headers = SplitCsvLine(line);

  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    TrimLineEnding(&line);
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    if (fields.size() != table.headers.size()) {
      throw std::runtime_error(path.string() + ":" + std::to_string(line_number) +
                               " has " + std::to_string(fields.size()) +
                               " fields for " + std::to_string(table.headers.size()) +
                               " headers");
    }

    CsvRow row;
    for (std::size_t index = 0; index < table.headers.size(); ++index) {
      row.emplace(table.headers[index], fields[index]);
    }
    table.rows.push_back(std::move(row));
  }

  return table;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string_view Field(const CsvRow& row, std::string_view column) {
  const auto found = row.find(std::string(column));
  return found == row.end() ? std::string_view{} : std::string_view(found->second);
}

bool HasColumn(const CsvTable& table, std::string_view column) {
  return std::find(table.headers.begin(), table.headers.end(), column) != table.headers.end();
}

bool RequireColumns(const CsvTable& table,
                    std::initializer_list<std::string_view> columns,
                    Harness* harness) {
  bool ok = true;
  for (const auto column : columns) {
    const bool present = HasColumn(table, column);
    harness->Check(present, table.path.filename().string() +
                                " missing required column " + std::string(column));
    ok &= present;
  }
  return ok;
}

std::unordered_map<std::string, const CsvRow*> IndexUnique(const CsvTable& table,
                                                           std::string_view column,
                                                           Harness* harness) {
  std::unordered_map<std::string, const CsvRow*> index;
  for (const auto& row : table.rows) {
    const std::string key(Field(row, column));
    harness->Check(!key.empty(), table.path.filename().string() +
                                     " row has empty " + std::string(column));
    if (key.empty()) continue;
    const auto inserted = index.emplace(key, &row);
    harness->Check(inserted.second, table.path.filename().string() +
                                        " duplicate " + std::string(column) +
                                        " " + key);
  }
  return index;
}

bool LooksLikeUuidV7(std::string_view uuid) {
  if (uuid.size() != 36) return false;
  for (const auto index : {8u, 13u, 18u, 23u}) {
    if (uuid[index] != '-') return false;
  }
  if (uuid[14] != '7') return false;
  return uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b' ||
         uuid[19] == 'A' || uuid[19] == 'B';
}

std::filesystem::path FindRepoRoot(std::filesystem::path start) {
  start = std::filesystem::absolute(start);
  while (!start.empty()) {
    if (std::filesystem::is_regular_file(
            start / "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv") &&
        std::filesystem::exists(start / "project/src")) {
      return start;
    }
    const auto parent = start.parent_path();
    if (parent == start) break;
    start = parent;
  }
  throw std::runtime_error("could not locate ScratchBird repo root");
}

std::filesystem::path FindUpgradeRoot(const std::filesystem::path& repo_root, char** argv) {
  if (argv != nullptr && argv[0] != nullptr) {
    const auto executable = std::filesystem::absolute(argv[0]);
    const auto sibling = executable.parent_path() / "UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv";
    if (std::filesystem::exists(sibling)) return executable.parent_path();
  }
  return repo_root / "project/tests/sbsql_parser_worker/generated/upgrade";
}

void ValidateFixtures(const CsvTable& fixtures, Harness* harness) {
  if (!RequireColumns(fixtures,
                      {"fixture_id", "area", "source_version", "target_version",
                       "compatibility_class", "expected_admission",
                       "expected_diagnostic", "required_evidence", "notes"},
                      harness)) {
    return;
  }

  const auto by_id = IndexUnique(fixtures, "fixture_id", harness);
  harness->Check(by_id.size() == fixtures.rows.size(), "fixture ids must be unique");

  const std::set<std::string> required_areas = {
      "parser_registry", "sblr_envelope", "catalog_seed",
      "fixture_regeneration", "database_open"};
  const std::set<std::string> allowed_classes = {
      "current", "additive", "read_only_compatible", "incompatible", "unsupported"};
  const std::set<std::string> allowed_admissions = {
      "admit", "admit_read_only", "refuse"};
  std::set<std::string> seen_areas;
  std::size_t refusal_rows = 0;
  std::size_t read_only_rows = 0;

  for (const auto& row : fixtures.rows) {
    const std::string id(Field(row, "fixture_id"));
    const std::string area(Field(row, "area"));
    const std::string compatibility_class(Field(row, "compatibility_class"));
    const std::string expected_admission(Field(row, "expected_admission"));
    const std::string expected_diagnostic(Field(row, "expected_diagnostic"));
    const std::string evidence(Field(row, "required_evidence"));

    harness->Check(StartsWith(id, "FSPE012H-"), id + " must use FSPE012H fixture id prefix");
    harness->Check(required_areas.count(area) == 1, id + " has unexpected area " + area);
    harness->Check(allowed_classes.count(compatibility_class) == 1,
                   id + " has unexpected compatibility_class " + compatibility_class);
    harness->Check(allowed_admissions.count(expected_admission) == 1,
                   id + " has unexpected expected_admission " + expected_admission);
    harness->Check(!evidence.empty(), id + " must name deterministic evidence");
    seen_areas.insert(area);

    if (expected_admission == "refuse") {
      ++refusal_rows;
      harness->Check(!expected_diagnostic.empty(), id + " refusal row lacks expected diagnostic");
      harness->Check(compatibility_class == "incompatible" || compatibility_class == "unsupported",
                     id + " refusal must be incompatible or unsupported");
    }
    if (expected_admission == "admit_read_only") {
      ++read_only_rows;
      harness->Check(!expected_diagnostic.empty(),
                     id + " read-only admission row must carry write-refusal diagnostic");
      harness->Check(compatibility_class == "read_only_compatible",
                     id + " read-only admission must be read_only_compatible");
    }
    if (compatibility_class == "current" || compatibility_class == "additive") {
      harness->Check(expected_admission == "admit",
                     id + " compatible current/additive rows should admit");
    }
  }

  for (const auto& area : required_areas) {
    harness->Check(seen_areas.count(area) == 1, "fixture matrix missing area " + area);
  }
  harness->Check(refusal_rows >= 4, "fixture matrix must include fail-closed refusal rows");
  harness->Check(read_only_rows >= 1, "fixture matrix must include read-only-compatible admission row");
}

void ValidateParserRegistryVersionEvidence(const std::filesystem::path& repo_root,
                                           Harness* harness) {
  const auto header = ReadText(repo_root / "project/src/server/parser_package_registry.hpp");
  const auto source = ReadText(repo_root / "project/src/server/parser_package_registry.cpp");
  const auto sbps = ReadText(repo_root / "project/src/server/sbps.hpp");

  harness->Check(Contains(header, "parser_api_major = 3"),
                 "parser package registry must declare parser_api_major 3");
  harness->Check(Contains(header, "sbps_min_major") && Contains(header, "sbps_max_major"),
                 "parser package registry must carry SBPS min/max version bounds");
  harness->Check(Contains(source, "values[\"format\"] != \"SBPPR1\""),
                 "parser registry loader must reject unsupported registry format");
  harness->Check(Contains(source, "SERVER.PARSER.REGISTRY_VERSION_UNSUPPORTED"),
                 "parser registry unsupported format diagnostic missing");
  harness->Check(Contains(source, "VersionInRange") &&
                     Contains(source, "minor < entry.sbps_min_minor") &&
                     Contains(source, "minor > entry.sbps_max_minor"),
                 "parser registry admission must check protocol version range");
  harness->Check(Contains(source, "hello.parser_api_major != entry.parser_api_major"),
                 "parser registry admission must reject parser API major mismatch");
  harness->Check(Contains(source, "SERVER.VERSION.INCOMPATIBLE"),
                 "parser version mismatch diagnostic missing");
  harness->Check(Contains(source, "StateAllowsAdmission") &&
                     Contains(source, "enabled") && Contains(source, "registered"),
                 "parser package state gate missing enabled/registered fail-closed check");
  harness->Check(Contains(sbps, "kProtocolMajor = 1") && Contains(sbps, "kProtocolMinor = 0"),
                 "SBPS protocol version constants missing");
}

void ValidateSblrEnvelopeVersionEvidence(const std::filesystem::path& repo_root,
                                         Harness* harness) {
  const auto internal_api_h = ReadText(repo_root / "project/src/engine/internal_api/engine_internal_api.hpp");
  const auto internal_api = ReadText(repo_root / "project/src/engine/internal_api/engine_internal_api.cpp");
  const auto engine_envelope_h = ReadText(repo_root / "project/src/engine/sblr/sblr_engine_envelope.hpp");
  const auto engine_envelope = ReadText(repo_root / "project/src/engine/sblr/sblr_engine_envelope.cpp");
  const auto server_admission = ReadText(repo_root / "project/src/server/sblr_admission.cpp");

  harness->Check(Contains(internal_api_h, "kSblrEnvelopeMajor = 1") &&
                     Contains(internal_api_h, "kSblrEnvelopeMinor = 0"),
                 "internal API SBLR envelope version constants missing");
  harness->Check(Contains(internal_api, "envelope.envelope_major != kSblrEnvelopeMajor") &&
                     Contains(internal_api, "envelope.envelope_minor > kSblrEnvelopeMinor"),
                 "internal API must accept only current major and supported-or-older minor");
  harness->Check(Contains(internal_api, "SB-ENGINE-API-UNSUPPORTED-SBLR-ENVELOPE-VERSION") &&
                     Contains(internal_api, "engine.api.unsupported_sblr_envelope_version"),
                 "internal API unsupported SBLR version diagnostic missing");
  harness->Check(Contains(engine_envelope_h, "kEngineSblrEnvelopeMajor = 1") &&
                     Contains(engine_envelope_h, "kEngineSblrEnvelopeMinor = 0"),
                 "engine SBLR envelope version constants missing");
  harness->Check(Contains(engine_envelope, "envelope.envelope_major != kEngineSblrEnvelopeMajor") &&
                     Contains(engine_envelope, "SBLR.OPERATION.VERSION_INVALID"),
                 "engine SBLR envelope major refusal missing");
  harness->Check(Contains(server_admission, "SBLRExecutionEnvelope.v3") &&
                     Contains(server_admission, "PARSER_SERVER_IPC.SBLR_ENVELOPE_VERSION_UNSUPPORTED"),
                 "server admission SBLRExecutionEnvelope.v3 refusal gate missing");
  harness->Check(Contains(server_admission, "ContainsSqlTextMarker") &&
                     Contains(server_admission, "SBLR.SQL_TEXT_FORBIDDEN"),
                 "server admission must reject SQL text at SBLR boundary");
  harness->Check(Contains(server_admission, "parser_resolved_names_to_uuids") &&
                     Contains(server_admission, "names_not_resolved_to_uuids"),
                 "server admission must require parser-resolved UUIDs");
}

struct FunctionSeedCounts {
  std::size_t runtime{0};
  std::size_t catalog{0};
  std::size_t names{0};
};

FunctionSeedCounts ValidateCanonicalFunctionSeeds(Harness* harness) {
  namespace fn = scratchbird::engine::functions;
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto runtime_entries = package.registry.Entries();
  const auto catalog_entries = package.catalog_registry.Entries();
  FunctionSeedCounts counts{runtime_entries.size(), catalog_entries.size(),
                            package.name_rows.size()};

  harness->Check(!runtime_entries.empty(), "live runtime function seed registry is empty");
  harness->Check(!catalog_entries.empty(), "live catalog function seed registry is empty");
  harness->Check(!package.name_rows.empty(), "live function name seed registry is empty");

  std::set<std::string> runtime_ids;
  std::set<std::string> runtime_uuids;
  for (const auto& entry : runtime_entries) {
    harness->Check(!entry.function_id.empty() && LooksLikeUuidV7(entry.function_uuid),
                   entry.function_id + " has an invalid live function identity");
    harness->Check(runtime_ids.insert(entry.function_id).second,
                   "duplicate live function id " + entry.function_id);
    harness->Check(runtime_uuids.insert(entry.function_uuid).second,
                   "duplicate live function UUID " + entry.function_uuid);
    const auto* by_id = package.registry.Lookup(entry.function_id);
    const auto* by_uuid = package.registry.LookupByUuid(entry.function_uuid);
    harness->Check(by_id != nullptr && by_uuid != nullptr &&
                       by_id->function_uuid == entry.function_uuid &&
                       by_uuid->function_id == entry.function_id,
                   entry.function_id + " does not round trip through exact live indexes");
    harness->Check(!entry.generated_row,
                   entry.function_id + " grants parser-generated function authority");
  }

  std::set<std::string> catalog_ids;
  std::set<std::string> catalog_uuids;
  for (const auto& entry : catalog_entries) {
    harness->Check(catalog_ids.insert(entry.function_id).second,
                   "duplicate catalog function id " + entry.function_id);
    harness->Check(catalog_uuids.insert(entry.function_uuid).second,
                   "duplicate catalog function UUID " + entry.function_uuid);
    const auto* runtime = package.registry.Lookup(entry.function_id);
    const auto* by_uuid = package.catalog_registry.LookupByUuid(entry.function_uuid);
    harness->Check(runtime != nullptr && by_uuid != nullptr &&
                       runtime->function_uuid == entry.function_uuid &&
                       by_uuid->function_id == entry.function_id,
                   entry.function_id + " changed identity across runtime/catalog seed indexes");
    harness->Check(entry.catalog_visible && !entry.generated_row,
                   entry.function_id + " has invalid catalog visibility or ownership");
  }

  std::set<std::string> seed_ids;
  std::set<std::string> named_catalog_ids;
  for (const auto& row : package.name_rows) {
    harness->Check(!row.name_lookup_seed_id.empty() &&
                       seed_ids.insert(row.name_lookup_seed_id).second,
                   "function name seed identity is empty or duplicated");
    harness->Check(!row.canonical_function_id.empty() &&
                       LooksLikeUuidV7(row.function_uuid) &&
                       !row.localized_name.empty() && !row.name_namespace.empty(),
                   row.name_lookup_seed_id + " function name seed is incomplete");
    const auto* catalog = package.catalog_registry.Lookup(row.canonical_function_id);
    const auto* runtime = package.registry.Lookup(row.canonical_function_id);
    harness->Check(catalog != nullptr && runtime != nullptr &&
                       catalog->function_uuid == row.function_uuid &&
                       runtime->function_uuid == row.function_uuid,
                   row.name_lookup_seed_id +
                       " does not bind one exact catalog/runtime function UUID");
    harness->Check(row.target_kind == "function" ||
                       row.target_kind == "function_alias",
                   row.name_lookup_seed_id + " has an invalid target kind");
    named_catalog_ids.insert(row.canonical_function_id);
  }
  for (const auto& entry : catalog_entries) {
    harness->Check(named_catalog_ids.contains(entry.function_id),
                   entry.function_id + " catalog seed has no live name seed");
  }
  return counts;
}

void ValidateFixtureRegenerationPolicy(const std::filesystem::path& repo_root, Harness* harness) {
  const auto manifest = ReadCsv(
      repo_root / "project/tests/sbsql_parser_worker/generated/repro/"
                  "DETERMINISTIC_ARTIFACT_MANIFEST.csv");
  if (RequireColumns(manifest,
                     {"artifact_path", "sha256", "size_bytes", "category", "source_inputs"},
                     harness)) {
    const auto by_path = IndexUnique(manifest, "artifact_path", harness);
    for (const auto required : {
             "project/tests/sbsql_parser_worker/generated/upgrade/"
             "UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv",
             "project/tests/sbsql_parser_worker/generated/upgrade/"
             "sbsql_upgrade_migration_compatibility_gate.cpp"}) {
      const auto found = by_path.find(required);
      harness->Check(found != by_path.end(),
                     std::string("deterministic manifest is missing ") + required);
      if (found == by_path.end()) continue;
      harness->Check(Field(*found->second, "category") == "test_generated_upgrade" &&
                         Field(*found->second, "source_inputs") ==
                             "repo_tracked_generated_fixture" &&
                         Field(*found->second, "sha256").size() == 64 &&
                         !Field(*found->second, "size_bytes").empty(),
                     std::string(required) + " has invalid deterministic provenance");
    }
  }

  const auto cache_h = ReadText(repo_root / "project/src/parsers/sbsql_worker/cache/sblr_template_cache.hpp");
  const auto cache_cpp = ReadText(repo_root / "project/src/parsers/sbsql_worker/cache/sblr_template_cache.cpp");
  for (const auto token : {"catalog_epoch", "security_policy_epoch", "grant_epoch",
                           "descriptor_epoch", "udr_epoch", "parser_profile",
                           "result_contract_hash"}) {
    harness->Check(Contains(cache_h, token), std::string("SBLR cache key missing ") + token);
    harness->Check(Contains(cache_cpp, token), std::string("SBLR cache stable key missing ") + token);
  }
  for (const auto token : {"InvalidateCatalogEpoch", "InvalidateSecurityPolicyEpoch",
                           "InvalidateGrantEpoch", "InvalidateDescriptorEpoch",
                           "InvalidateUdrEpoch", "InvalidateParserProfile",
                           "InvalidateResultContractHash"}) {
    harness->Check(Contains(cache_h, token) && Contains(cache_cpp, token),
                   std::string("SBLR cache invalidation hook missing ") + token);
  }
}

void ValidateDatabaseCompatibilityEvidence(const std::filesystem::path& repo_root,
                                           const std::filesystem::path& artifact_root,
                                           Harness* harness) {
  const auto db_format_h = ReadText(repo_root / "project/src/storage/disk/database_format.hpp");
  const auto db_format_cpp = ReadText(repo_root / "project/src/storage/disk/database_format.cpp");
  const auto db_lifecycle_h = ReadText(repo_root / "project/src/storage/database/database_lifecycle.hpp");
  const auto db_lifecycle = ReadText(repo_root / "project/src/storage/database/database_lifecycle.cpp");
  const auto report = ReadText(artifact_root / "UPGRADE_MIGRATION_COMPATIBILITY_REPORT.md");

  harness->Check(Contains(db_format_h, "kScratchBirdDatabaseFormatMajor = 1") &&
                     Contains(db_format_h, "kScratchBirdDatabaseFormatMinor = 0"),
                 "database format version constants missing");
  harness->Check(Contains(db_format_cpp, "FORMAT.VERSION_UNSUPPORTED") &&
                     Contains(db_format_cpp, "storage.database.format_version_unsupported"),
                 "database format unsupported version diagnostic must be canonical");
  harness->Check(Contains(db_format_cpp, "FORMAT.UNKNOWN_REQUIRED_FLAG") &&
                     Contains(db_format_cpp, "storage.database.unknown_required_compatibility_flag"),
                 "database format unknown required flag diagnostic missing");
  harness->Check(Contains(db_format_h, "requires_cluster_authority") &&
                     Contains(db_format_h, "requires_decryption_password") &&
                     Contains(db_format_h, "unknown_page_safe_classification_required"),
                 "database compatibility flags missing fail-closed open evidence");
  harness->Check(Contains(db_lifecycle_h, "DatabaseOpenCompatibilityClass") &&
                     Contains(db_lifecycle_h, "upgrade_required") &&
                     Contains(db_lifecycle_h, "read_only_compatible") &&
                     Contains(db_lifecycle_h, "unsupported"),
                 "database lifecycle must expose open compatibility classification");
  harness->Check(Contains(db_lifecycle_h, "read_only_open") &&
                     Contains(db_lifecycle_h, "write_admission_fenced") &&
                     Contains(db_lifecycle_h, "startup_recovery_classification"),
                 "database lifecycle state must expose read-only/write-fence/recovery classification");
  harness->Check(Contains(db_lifecycle, "ClassifyDatabaseOpenCompatibility") &&
                     Contains(db_lifecycle, "FORMAT.UPGRADE_REQUIRED") &&
                     Contains(db_lifecycle, "format_upgrade_required"),
                 "database open must classify upgrade-required databases before mutation");
  harness->Check(Contains(db_lifecycle, "config.read_only ? FileOpenMode::open_existing_read_only") &&
                     Contains(db_lifecycle, "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED"),
                 "database open must support read-only open and refuse unsafe writable open");
  harness->Check(Contains(db_lifecycle, "cluster_authority_available") &&
                     Contains(db_lifecycle, "SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED"),
                 "database open must fail closed when cluster authority is required but absent");
  harness->Check(Contains(db_lifecycle, "requires_decryption_password") &&
                     Contains(db_lifecycle, "SB-DB-LIFECYCLE-DECRYPTION-REQUIRED"),
                 "database open must fail closed when decryption is required but unavailable");
  harness->Check(Contains(db_lifecycle, "ResourceSeedPackMismatch") &&
                     Contains(db_lifecycle, "expected_resource_seed_pack_content_hash") &&
                     Contains(db_lifecycle, "seed_pack_upgrade_required"),
                 "database open must compare seed-pack version/hash against expected policy");
  harness->Check(Contains(db_lifecycle, "ReadTransactionInventoryPage") &&
                     Contains(db_lifecycle, "write_admission_must_remain_fenced"),
                 "database open must classify MGA transaction inventory before write admission");
  harness->Check(!Contains(LowerAscii(db_lifecycle), "wal recovery"),
                 "database lifecycle must not introduce WAL recovery authority");

  for (const auto token : {"current, upgrade-required, read-only-compatible, or unsupported",
                           "unsupported database opened as writable is a blocker",
                           "database version matrix",
                           "SBLR envelope compatibility matrix"}) {
    harness->Check(Contains(report, token),
                   std::string("upgrade compatibility report missing token ") + token);
  }
}

void ValidateEngineBoundaryInvariants(const std::filesystem::path& repo_root, Harness* harness) {
  const auto server_admission = ReadText(repo_root / "project/src/server/sblr_admission.cpp");
  const auto internal_api = ReadText(repo_root / "project/src/engine/internal_api/engine_internal_api.cpp");
  const auto no_wal_gate = ReadText(repo_root / "project/tests/sbsql_parser_worker/generated/hardening/"
                                                "sbsql_no_spin_no_wal_no_direct_db_gate.cpp");
  const auto sblr_contract = ReadText(repo_root / "public_contract_snapshot");
  const auto replay = ReadText(repo_root / "project/tests/sbsql_parser_worker/generated/replay/"
                                           "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv");

  harness->Check(Contains(server_admission, "The engine accepts canonical SBLR only.") &&
                     Contains(server_admission, "raw_sql_forbidden"),
                 "server admission must preserve no-SQL engine boundary");
  harness->Check(Contains(internal_api, "contains_sql_text") &&
                     Contains(internal_api, "SB-ENGINE-API-SQL-TEXT-NOT-ACCEPTED"),
                 "engine API must refuse SQL text in SBLR envelope");
  harness->Check(Contains(internal_api, "parser_is_trusted") &&
                     Contains(internal_api, "SB-ENGINE-API-PARSER-MUST-NOT-BE-TRUSTED"),
                 "engine API must treat parser registry as evidence, not authority");
  harness->Check(Contains(no_wal_gate, "no_wal_recovery=true") &&
                     Contains(no_wal_gate, "wal_recovery_forbidden"),
                 "hardening gate must retain anti-WAL evidence");
  harness->Check(Contains(sblr_contract, "ScratchBird MGA is the controlling transaction") &&
                     Contains(sblr_contract, "WAL/redo/undo is not authoritative recovery") &&
                     Contains(sblr_contract, "SBLR public execution contract snapshot"),
                 "canonical specs must retain MGA-not-WAL compatibility authority");
  harness->Check(Contains(replay, "execute-sblr-internal-procedure-only-no-sql-text"),
                 "replay fixtures must assert SBLR/internal procedure only boundary");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto repo_root = FindRepoRoot(argc > 1 ? std::filesystem::path(argv[1])
                                                : std::filesystem::current_path());
    const auto artifact_root =
        argc > 2 ? std::filesystem::path(argv[2])
                 : repo_root / "project/tests/sbsql_parser_worker/fixtures/"
                               "full_parser_udr_engine/artifacts";
    const auto upgrade_root = FindUpgradeRoot(repo_root, argv);
    const auto fixtures = ReadCsv(upgrade_root / "UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv");

    Harness harness;
    ValidateFixtures(fixtures, &harness);
    ValidateParserRegistryVersionEvidence(repo_root, &harness);
    ValidateSblrEnvelopeVersionEvidence(repo_root, &harness);
    const auto seed_counts = ValidateCanonicalFunctionSeeds(&harness);
    ValidateFixtureRegenerationPolicy(repo_root, &harness);
    ValidateDatabaseCompatibilityEvidence(repo_root, artifact_root, &harness);
    ValidateEngineBoundaryInvariants(repo_root, &harness);

    std::cout << "{\n";
    std::cout << "  \"ok\": " << (harness.ok ? "true" : "false") << ",\n";
    std::cout << "  \"gate\": \"sbsql_upgrade_migration_compatibility_gate\",\n";
    std::cout << "  \"fixture_rows\": " << fixtures.rows.size() << ",\n";
    std::cout << "  \"runtime_function_rows\": " << seed_counts.runtime << ",\n";
    std::cout << "  \"catalog_function_rows\": " << seed_counts.catalog << ",\n";
    std::cout << "  \"name_seed_rows\": " << seed_counts.names << ",\n";
    std::cout << "  \"parser_registry_version_evidence\": \"checked\",\n";
    std::cout << "  \"sblr_envelope_version_evidence\": \"checked\",\n";
    std::cout << "  \"database_compatibility_evidence\": \"checked\",\n";
    std::cout << "  \"engine_boundary\": \"sblr_internal_only_no_sql_mga_not_wal\",\n";
    std::cout << "  \"failures\": " << harness.failures << "\n";
    std::cout << "}\n";

    return harness.ok ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << "sbsql_upgrade_migration_compatibility_gate failed: "
              << ex.what() << '\n';
    return 1;
  }
}
