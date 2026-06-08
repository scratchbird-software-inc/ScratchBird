// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "registry/function_seed_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using CsvRow = std::unordered_map<std::string, std::string>;

struct CsvTable {
  std::filesystem::path path;
  std::vector<std::string> headers;
  std::vector<CsvRow> rows;
};

struct BuiltinRow {
  std::string builtin_id;
  std::string builtin_uuid;
  std::string kind;
  std::string status;
  std::string engine_entrypoint;
};

using BuiltinIndex = std::unordered_map<std::string, BuiltinRow>;

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

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

std::string TrimAscii(std::string_view value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' ||
          value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
          value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

std::string YamlValueAfterColon(std::string_view line) {
  const auto pos = line.find(':');
  if (pos == std::string_view::npos) return {};
  auto value = TrimAscii(line.substr(pos + 1));
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
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

bool CsvRecordComplete(std::string_view record) {
  bool in_quotes = false;
  for (std::size_t i = 0; i < record.size(); ++i) {
    const char ch = record[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < record.size() && record[i + 1] == '"') {
          ++i;
        } else {
          in_quotes = false;
        }
      }
      continue;
    }
    if (ch == '"') in_quotes = true;
  }
  return !in_quotes;
}

bool ReadCsvRecord(std::istream& input,
                   std::string* record,
                   std::size_t* physical_line_number) {
  record->clear();
  std::string line;
  while (std::getline(input, line)) {
    ++(*physical_line_number);
    TrimLineEnding(&line);
    if (!record->empty()) record->push_back('\n');
    record->append(line);
    if (CsvRecordComplete(*record)) return true;
  }
  return !record->empty();
}

CsvTable ReadCsv(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());

  std::string line;
  std::size_t line_number = 0;
  if (!ReadCsvRecord(input, &line, &line_number)) {
    throw std::runtime_error("empty CSV " + path.string());
  }

  CsvTable table;
  table.path = path;
  table.headers = SplitCsvLine(line);

  while (ReadCsvRecord(input, &line, &line_number)) {
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

BuiltinIndex ReadBuiltinExpressionRegistry(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("failed to open " + path.string());

  BuiltinIndex index;
  BuiltinRow current;
  auto commit = [&]() {
    if (current.builtin_id.empty()) return;
    if (current.builtin_uuid.empty()) {
      throw std::runtime_error("builtin row missing builtin_uuid for " + current.builtin_id);
    }
    const auto [_, inserted] = index.emplace(current.builtin_id, current);
    if (!inserted) throw std::runtime_error("duplicate builtin_id " + current.builtin_id);
    current = BuiltinRow{};
  };

  std::string line;
  while (std::getline(input, line)) {
    TrimLineEnding(&line);
    if (StartsWith(line, "- builtin_id: ")) {
      commit();
      current.builtin_id = YamlValueAfterColon(line);
    } else if (!current.builtin_id.empty() && StartsWith(line, "  builtin_uuid: ")) {
      current.builtin_uuid = YamlValueAfterColon(line);
    } else if (!current.builtin_id.empty() && StartsWith(line, "  kind: ")) {
      current.kind = YamlValueAfterColon(line);
    } else if (!current.builtin_id.empty() && StartsWith(line, "  status: ")) {
      current.status = YamlValueAfterColon(line);
    } else if (!current.builtin_id.empty() && StartsWith(line, "  engine_entrypoint: ")) {
      current.engine_entrypoint = YamlValueAfterColon(line);
    }
  }
  commit();
  return index;
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

void MergeGate011FunctionOracleRows(const CsvTable& function_oracle,
                                    const CsvTable& release_declaration,
                                    BuiltinIndex* index,
                                    Harness* harness) {
  const auto release_by_surface = IndexUnique(release_declaration, "surface_id", harness);
  for (const auto& row : function_oracle.rows) {
    const std::string surface_id(Field(row, "surface_id"));
    if (surface_id != "SBSQL-6E2D0E0B0110" && surface_id != "SBSQL-6E2D0E0B0111") {
      continue;
    }

    harness->Check(Field(row, "oracle_authority_status") == "full_oracle",
                   surface_id + " Gate 011 function oracle is not full_oracle");
    const auto release = release_by_surface.find(surface_id);
    harness->Check(release != release_by_surface.end(),
                   surface_id + " missing from SBSQL_SURFACE_RELEASE_DECLARATION.csv");
    if (release == release_by_surface.end() ||
        Field(row, "oracle_authority_status") != "full_oracle") {
      continue;
    }

    BuiltinRow builtin;
    builtin.builtin_id = std::string(Field(row, "matched_builtin_id"));
    builtin.builtin_uuid = std::string(Field(*release->second, "fixed_uuid_v7"));
    builtin.kind = "scalar";
    builtin.status = "accepted";
    builtin.engine_entrypoint = std::string(Field(row, "engine_entrypoint"));

    harness->Check(!builtin.builtin_id.empty(), surface_id + " missing matched_builtin_id");
    harness->Check(StartsWith(builtin.builtin_id, "sb.scalar."),
                   surface_id + " Gate 011 builtin id is not scalar");
    harness->Check(LooksLikeUuidV7(builtin.builtin_uuid),
                   surface_id + " Gate 011 function UUID is not UUIDv7-compatible");
    harness->Check(!builtin.engine_entrypoint.empty(),
                   surface_id + " Gate 011 function missing engine_entrypoint");
    if (builtin.builtin_id.empty()) continue;

    const auto existing = index->find(builtin.builtin_id);
    if (existing != index->end()) {
      harness->Check(existing->second.builtin_uuid == builtin.builtin_uuid,
                     "Gate 011 builtin UUID mismatch " + builtin.builtin_id);
      harness->Check(existing->second.kind == builtin.kind,
                     "Gate 011 builtin kind mismatch " + builtin.builtin_id);
      harness->Check(existing->second.status == builtin.status,
                     "Gate 011 builtin status mismatch " + builtin.builtin_id);
      harness->Check(existing->second.engine_entrypoint == builtin.engine_entrypoint,
                     "Gate 011 builtin entrypoint mismatch " + builtin.builtin_id);
      continue;
    }
    index->emplace(builtin.builtin_id, builtin);
  }
}

std::filesystem::path FindRepoRoot(std::filesystem::path start) {
  start = std::filesystem::absolute(start);
  while (!start.empty()) {
    if (std::filesystem::exists(start / "public_release_evidence") &&
        std::filesystem::exists(start / "project/src/engine/functions/registry")) {
      return start;
    }
    const auto parent = start.parent_path();
    if (parent == start) break;
    start = parent;
  }
  throw std::runtime_error("could not locate ScratchBird repo root");
}

void ValidateFixedRegistry(const CsvTable& fixed, Harness* harness) {
  if (!RequireColumns(fixed,
                      {"canonical_function_id", "function_uuid", "uuid_kind",
                       "uuid_generation_rule", "semantic_stability",
                       "catalog_row_uuid_rule"},
                      harness)) {
    return;
  }
  const auto by_id = IndexUnique(fixed, "canonical_function_id", harness);
  const auto by_uuid = IndexUnique(fixed, "function_uuid", harness);
  harness->Check(by_id.size() == fixed.rows.size(), "fixed registry has duplicate function ids");
  harness->Check(by_uuid.size() == fixed.rows.size(), "fixed registry has duplicate UUIDs");
  for (const auto& row : fixed.rows) {
    const auto id = Field(row, "canonical_function_id");
    const auto uuid = Field(row, "function_uuid");
    harness->Check(StartsWith(id, "sb.fn."), std::string(id) + " is not a canonical sb.fn id");
    harness->Check(LooksLikeUuidV7(uuid), std::string(id) + " function_uuid is not UUIDv7-compatible");
    harness->Check(Field(row, "uuid_kind") == "fixed_uuidv7_compatible_builtin_function_object",
                   std::string(id) + " has wrong uuid_kind");
    harness->Check(Field(row, "uuid_generation_rule") ==
                       "reserved_timestamp_2026_01_01_ms_plus_sha256_canonical_function_id",
                   std::string(id) + " has wrong uuid generation rule");
    harness->Check(Contains(Field(row, "semantic_stability"), "retain while semantics remain compatible"),
                   std::string(id) + " lacks semantic stability rule");
    harness->Check(Contains(Field(row, "catalog_row_uuid_rule"), "row_uuid is distinct from fixed function_uuid"),
                   std::string(id) + " does not separate row_uuid from function_uuid");
  }
}

void ValidateEngineSeedPackage(
    const CsvTable& fixed,
    const CsvTable& names,
    const std::unordered_map<std::string, const CsvRow*>& fixed_by_id,
    const BuiltinIndex& builtin_by_id,
    Harness* harness) {
  namespace fn = scratchbird::engine::functions;
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto runtime_entries = package.registry.Entries();
  const auto catalog_entries = package.catalog_registry.Entries();
  harness->Check(catalog_entries.size() == fixed.rows.size(),
                 "engine catalog seed registry row count does not match fixed registry");

  std::map<std::string, std::string> catalog_uuid_by_id;
  std::set<std::string> catalog_uuids;
  for (const auto& entry : catalog_entries) {
    harness->Check(!entry.function_id.empty(), "engine catalog seed entry has empty function_id");
    harness->Check(!entry.function_uuid.empty(), entry.function_id + " has empty function_uuid");
    harness->Check(catalog_uuid_by_id.emplace(entry.function_id, entry.function_uuid).second,
                   "engine catalog seed duplicate function_id " + entry.function_id);
    harness->Check(catalog_uuids.insert(entry.function_uuid).second,
                   "engine catalog seed duplicate function_uuid " + entry.function_uuid);
    const auto fixed_row = fixed_by_id.find(entry.function_id);
    harness->Check(fixed_row != fixed_by_id.end(),
                   "engine catalog seed contains non-canonical function_id " + entry.function_id);
    if (fixed_row != fixed_by_id.end()) {
      harness->Check(entry.function_uuid == Field(*fixed_row->second, "function_uuid"),
                     entry.function_id + " catalog UUID does not match canonical fixed registry");
    }
    harness->Check(!entry.generated_row, entry.function_id + " incorrectly marks catalog seed as parser generated row");
    harness->Check(entry.catalog_visible, entry.function_id + " catalog seed must remain catalog-visible");
  }

  for (const auto& row : fixed.rows) {
    const std::string id(Field(row, "canonical_function_id"));
    harness->Check(catalog_uuid_by_id.count(id) == 1, "engine catalog seed missing " + id);
  }

  std::map<std::string, std::string> runtime_uuid_by_id;
  std::set<std::string> runtime_uuids;
  std::size_t fixed_runtime_rows = 0;
  std::size_t builtin_runtime_rows = 0;
  for (const auto& entry : runtime_entries) {
    harness->Check(!entry.function_id.empty(), "engine runtime seed entry has empty function_id");
    harness->Check(!entry.function_uuid.empty(), entry.function_id + " has empty runtime function_uuid");
    harness->Check(LooksLikeUuidV7(entry.function_uuid),
                   entry.function_id + " runtime function_uuid is not UUIDv7-compatible");
    harness->Check(runtime_uuid_by_id.emplace(entry.function_id, entry.function_uuid).second,
                   "engine runtime seed duplicate function_id " + entry.function_id);
    harness->Check(runtime_uuids.insert(entry.function_uuid).second,
                   "engine runtime seed duplicate function_uuid " + entry.function_uuid);
    harness->Check(!entry.generated_row, entry.function_id + " incorrectly marks runtime seed as parser generated row");

    const auto fixed_row = fixed_by_id.find(entry.function_id);
    if (fixed_row != fixed_by_id.end()) {
      ++fixed_runtime_rows;
      harness->Check(entry.function_uuid == Field(*fixed_row->second, "function_uuid"),
                     entry.function_id + " runtime UUID does not match canonical fixed registry");
      continue;
    }

    const auto builtin = builtin_by_id.find(entry.function_id);
    harness->Check(builtin != builtin_by_id.end(),
                   "engine runtime seed contains untracked builtin function_id " + entry.function_id);
    if (builtin == builtin_by_id.end()) continue;
    ++builtin_runtime_rows;
    harness->Check(entry.function_uuid == builtin->second.builtin_uuid,
                   entry.function_id + " runtime UUID does not match builtin-expression-registry.yaml");
    harness->Check(LooksLikeUuidV7(builtin->second.builtin_uuid),
                   entry.function_id + " builtin UUID is not UUIDv7-compatible");
    harness->Check(builtin->second.status == "accepted",
                   entry.function_id + " runtime builtin is not accepted");
    harness->Check(!builtin->second.kind.empty(), entry.function_id + " builtin kind is empty");
    harness->Check(!builtin->second.engine_entrypoint.empty(),
                   entry.function_id + " builtin engine_entrypoint is empty");
  }

  harness->Check(fixed_runtime_rows == fixed.rows.size(),
                 "engine runtime registry is missing fixed catalog function rows");
  harness->Check(builtin_runtime_rows > 0,
                 "engine runtime registry did not validate any builtin-expression rows");
  for (const auto& [builtin_id, builtin] : builtin_by_id) {
    if (builtin.status == "accepted" && builtin.kind == "scalar") {
      harness->Check(runtime_uuid_by_id.count(builtin_id) == 1,
                     builtin_id + " accepted scalar builtin missing from runtime seed registry");
    }
  }

  harness->Check(package.name_rows.size() == names.rows.size(),
                 "engine name seed row count does not match FUNCTION_NAME_LOOKUP_SEED_MATRIX.csv");
  std::map<std::string, const fn::FunctionNameSeedRow*> engine_name_by_seed_id;
  for (const auto& row : package.name_rows) {
    harness->Check(!row.name_lookup_seed_id.empty(), "engine name seed missing seed id");
    harness->Check(engine_name_by_seed_id.emplace(row.name_lookup_seed_id, &row).second,
                   "engine name seed duplicate id " + row.name_lookup_seed_id);
  }
  for (const auto& row : names.rows) {
    const std::string seed_id(Field(row, "name_lookup_seed_id"));
    const auto found = engine_name_by_seed_id.find(seed_id);
    harness->Check(found != engine_name_by_seed_id.end(),
                   "engine name seed missing " + seed_id);
    if (found == engine_name_by_seed_id.end()) continue;
    const auto& engine = *found->second;
    harness->Check(engine.function_uuid == Field(row, "function_uuid"), seed_id + " UUID mismatch");
    harness->Check(engine.canonical_function_id == Field(row, "canonical_function_id"),
                   seed_id + " canonical_function_id mismatch");
    harness->Check(engine.name_namespace == Field(row, "namespace"), seed_id + " namespace mismatch");
    harness->Check(engine.language == Field(row, "language"), seed_id + " language mismatch");
    harness->Check(engine.name_class == Field(row, "name_class"), seed_id + " name_class mismatch");
    harness->Check(engine.localized_name == Field(row, "name"), seed_id + " name mismatch");
    harness->Check(engine.target_kind == Field(row, "target_kind"), seed_id + " target_kind mismatch");
    harness->Check(engine.parser_profile == Field(row, "parser_profile"), seed_id + " parser_profile mismatch");
  }
}

void ValidateNameRows(
    const CsvTable& names,
    const std::unordered_map<std::string, const CsvRow*>& fixed_by_id,
    const std::unordered_map<std::string, const CsvRow*>& fixed_by_uuid,
    Harness* harness) {
  if (!RequireColumns(names,
                      {"name_lookup_seed_id", "function_uuid", "canonical_function_id",
                       "namespace", "language", "name_class", "name", "target_kind",
                       "parser_profile", "notes"},
                      harness)) {
    return;
  }
  IndexUnique(names, "name_lookup_seed_id", harness);
  std::map<std::string, std::set<std::string>> classes_by_function;
  std::size_t donor_alias_count = 0;
  std::size_t plugin_alias_count = 0;
  for (const auto& row : names.rows) {
    const auto id = Field(row, "canonical_function_id");
    const auto uuid = Field(row, "function_uuid");
    const auto name_class = Field(row, "name_class");
    const auto target_kind = Field(row, "target_kind");
    const auto name = Field(row, "name");
    const auto fixed_id = fixed_by_id.find(std::string(id));
    const auto fixed_uuid = fixed_by_uuid.find(std::string(uuid));
    harness->Check(fixed_id != fixed_by_id.end(), std::string(id) + " name row has no fixed registry id");
    harness->Check(fixed_uuid != fixed_by_uuid.end(), std::string(id) + " name row has no fixed registry UUID");
    if (fixed_id != fixed_by_id.end()) {
      harness->Check(uuid == Field(*fixed_id->second, "function_uuid"),
                     std::string(id) + " name row UUID disagrees with fixed registry");
    }
    classes_by_function[std::string(id)].insert(std::string(name_class));

    if (name_class == "canonical_name") {
      harness->Check(Field(row, "namespace") == "sys.fn", std::string(id) + " canonical name not in sys.fn");
      harness->Check(target_kind == "function", std::string(id) + " canonical name must target function");
      harness->Check(name == std::string(id).substr(std::string("sb.fn.").size()),
                     std::string(id) + " canonical label must be derived from canonical_function_id");
    } else if (name_class == "sbsql_default_name") {
      harness->Check(target_kind == "function", std::string(id) + " SBSQL default name must target function");
      harness->Check(Field(row, "parser_profile") == "sbsql",
                     std::string(id) + " SBSQL default name must be parser-profile scoped");
    } else if (name_class == "donor_alias") {
      ++donor_alias_count;
      harness->Check(target_kind == "function_alias", std::string(id) + " donor alias must be a label alias");
      harness->Check(StartsWith(Field(row, "namespace"), "sys.fn.compat."),
                     std::string(id) + " donor alias namespace must be compatibility-scoped");
      harness->Check(Contains(Field(row, "notes"), "not durable authority"),
                     std::string(id) + " donor alias must state it is not durable authority");
    } else if (name_class == "plugin_alias") {
      ++plugin_alias_count;
      harness->Check(target_kind == "function_alias", std::string(id) + " plugin alias must be a label alias");
      harness->Check(StartsWith(Field(row, "namespace"), "sys.fn.compat."),
                     std::string(id) + " plugin alias namespace must be compatibility-scoped");
      harness->Check(Contains(Field(row, "notes"), "not durable authority"),
                     std::string(id) + " plugin alias must state it is not durable authority");
    } else {
      harness->Check(false, std::string(id) + " has unsupported name_class " + std::string(name_class));
    }
  }

  for (const auto& [id, classes] : classes_by_function) {
    harness->Check(classes.count("canonical_name") == 1, id + " missing canonical_name label");
    harness->Check(classes.count("sbsql_default_name") == 1, id + " missing sbsql_default_name label");
  }
  harness->Check(donor_alias_count > 0, "name matrix has no donor aliases");
  harness->Check(plugin_alias_count > 0, "name matrix has no plugin aliases");
}

void ValidateCatalogRequirements(
    const CsvTable& catalog,
    const std::unordered_map<std::string, const CsvRow*>& fixed_by_id,
    Harness* harness) {
  if (!RequireColumns(catalog,
                      {"catalog_requirement_id", "canonical_function_id", "function_uuid",
                       "required_catalog_objects", "uuid_requirements",
                       "namespace_requirements", "descriptor_requirements"},
                      harness)) {
    return;
  }
  IndexUnique(catalog, "catalog_requirement_id", harness);
  harness->Check(catalog.rows.size() == fixed_by_id.size(),
                 "catalog requirements row count does not match fixed registry");
  for (const auto& row : catalog.rows) {
    const std::string id(Field(row, "canonical_function_id"));
    const auto fixed = fixed_by_id.find(id);
    harness->Check(fixed != fixed_by_id.end(), id + " catalog requirement has no fixed registry row");
    if (fixed != fixed_by_id.end()) {
      harness->Check(Field(row, "function_uuid") == Field(*fixed->second, "function_uuid"),
                     id + " catalog requirement UUID disagrees with fixed registry");
    }
    const auto required = Field(row, "required_catalog_objects");
    for (const auto object_name : {"sys.catalog.functions", "sys.catalog.function_signatures",
                                   "sys.catalog.function_names", "sys.catalog.function_aliases"}) {
      harness->Check(Contains(required, object_name), id + " missing required catalog object " + object_name);
    }
    harness->Check(Contains(Field(row, "uuid_requirements"), "fixed function_uuid"),
                   id + " missing fixed function_uuid requirement");
    harness->Check(Contains(Field(row, "uuid_requirements"), "row_uuid"),
                   id + " missing database-create row_uuid requirement");
    harness->Check(Field(row, "namespace_requirements") == "sys.fn",
                   id + " namespace requirement is not sys.fn");
    harness->Check(!Field(row, "descriptor_requirements").empty(),
                   id + " descriptor requirements must not be empty");
  }
}

void ValidateParserRegistryEvidence(const std::filesystem::path& repo_root, Harness* harness) {
  const auto header = ReadText(repo_root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp");
  const auto source = ReadText(repo_root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp");
  harness->Check(Contains(header, "GeneratedSurfaceRegistryRow"), "parser registry evidence header missing row type");
  harness->Check(Contains(header, "fixed_uuid_v7"), "parser registry evidence missing fixed_uuid_v7 field");
  harness->Check(Contains(header, "kGeneratedSurfaceRegistryRowCount = 2617"),
                 "parser registry evidence row count changed from corrected authority baseline");
  harness->Check(Contains(source, "canonical_spec_plus_sblr_matrix"),
                 "parser registry evidence no longer references canonical/SBLR matrix evidence");
  harness->Check(!Contains(source, "019b76da-a800-"),
                 "parser registry evidence must not reuse canonical function fixed UUID seed prefix");
  harness->Check(!Contains(source, "sb.fn."),
                 "parser registry evidence must not claim canonical function id authority");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto repo_root = FindRepoRoot(argc > 1 ? std::filesystem::path(argv[1])
                                                : std::filesystem::current_path());
    const auto canonical_root =
        repo_root / "public_input_snapshot";
    const auto fixed = ReadCsv(canonical_root / "FIXED_FUNCTION_UUID_REGISTRY.csv");
    const auto names = ReadCsv(canonical_root / "FUNCTION_NAME_LOOKUP_SEED_MATRIX.csv");
    const auto catalog = ReadCsv(canonical_root / "CATALOG_OBJECT_REQUIREMENTS.csv");
    auto builtins =
        ReadBuiltinExpressionRegistry(repo_root / "public_contract_snapshot");
    const auto special_builtins =
        ReadBuiltinExpressionRegistry(repo_root / "public_contract_snapshot");
    for (const auto& [builtin_id, builtin] : special_builtins) {
      const auto [_, inserted] = builtins.emplace(builtin_id, builtin);
      if (!inserted) {
        throw std::runtime_error("duplicate builtin authority across expression and special-form registries: " +
                                 builtin_id);
      }
    }

    Harness harness;
    const auto function_oracle = ReadCsv(
        repo_root / "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/FUNCTION_SEMANTIC_ORACLE_MATRIX.csv");
    const auto release_declaration = ReadCsv(
        repo_root / "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/SBSQL_SURFACE_RELEASE_DECLARATION.csv");
    MergeGate011FunctionOracleRows(function_oracle, release_declaration, &builtins, &harness);
    ValidateFixedRegistry(fixed, &harness);
    const auto fixed_by_id = IndexUnique(fixed, "canonical_function_id", &harness);
    const auto fixed_by_uuid = IndexUnique(fixed, "function_uuid", &harness);
    ValidateNameRows(names, fixed_by_id, fixed_by_uuid, &harness);
    ValidateCatalogRequirements(catalog, fixed_by_id, &harness);
    ValidateEngineSeedPackage(fixed, names, fixed_by_id, builtins, &harness);
    ValidateParserRegistryEvidence(repo_root, &harness);

    std::cout << "{\n";
    std::cout << "  \"ok\": " << (harness.ok ? "true" : "false") << ",\n";
    std::cout << "  \"gate\": \"sbsql_fixed_uuid_catalog_seed_gate\",\n";
    std::cout << "  \"fixed_function_rows\": " << fixed.rows.size() << ",\n";
    std::cout << "  \"name_seed_rows\": " << names.rows.size() << ",\n";
    std::cout << "  \"catalog_requirement_rows\": " << catalog.rows.size() << ",\n";
    std::cout << "  \"builtin_expression_and_special_form_rows\": " << builtins.size() << ",\n";
    std::cout << "  \"parser_registry_evidence\": \"checked_as_non_authority\",\n";
    std::cout << "  \"failures\": " << harness.failures << "\n";
    std::cout << "}\n";
    return harness.ok ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
