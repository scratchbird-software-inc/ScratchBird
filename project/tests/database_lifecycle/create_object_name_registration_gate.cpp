// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_CREATE_REGISTERS_ALL_OBJECT_NAMES
// Owner direction 2026-10-30: CREATE generates binary(16) UUIDv7 identities and
// updates the name lookup structures for every SQL object it makes. This gate
// reads a freshly created file and checks:
//  - every object identity is a unique, non-nil binary UUIDv7;
//  - every named SQL object the file holds resolves through the engine's name
//    registry to its own UUID, with its class and simple name, in English.
// The expected names are re-derived here from the records themselves, not from
// the code under test. The directory is removed at exit, including on failure.

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_security_record_codec.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "page_manager.hpp"
#include "time.hpp"
#include "uuid.hpp"
#include "../support/engine_statement_fixture.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace api = scratchbird::engine::internal_api;

std::filesystem::path g_directory;
void RemoveDirectory() {
  if (g_directory.empty()) return;
  std::error_code ignored;
  std::filesystem::remove_all(g_directory, ignored);
}

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "FAIL create_object_name_registration: " << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

bool IsV7(const platform::Uuid& id) {
  return (id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80;
}

std::string Field(const std::string& payload, std::string_view key) {
  std::size_t at = 0;
  while (at < payload.size()) {
    const auto end = payload.find('\n', at);
    const auto line = payload.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 && line[key.size()] == '=')
      return line.substr(key.size() + 1);
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return {};
}

struct Expected {
  platform::Uuid object;
  std::string object_class;
  std::string name;
  platform::Uuid parent;
};

}  // namespace

int main() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "sb-create-names-XXXXXX").string();
  Require(mkdtemp(pattern.data()) != nullptr, "fixture directory create failed");
  g_directory = pattern;
  std::atexit(RemoveDirectory);
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
              "create_object_name_registration_gate").ok(),
          "fixture memory manager configuration failed");
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Require(clock.ok() && millis.ok(), "node clock unavailable");

  db::DatabaseCreateConfig create;
  create.path = (g_directory / "names.sbdb").string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::database, millis.unix_epoch_millis).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, millis.unix_epoch_millis + 1).value;
  create.creation_unix_epoch_millis = millis.unix_epoch_millis;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << '\n';
  Require(created.ok() && created.creation_ready_for_handoff(), "create failed");

  // Read every record the file holds.
  disk::FileDevice device;
  Require(device.Open(create.path, disk::FileOpenMode::open_existing_read_only).ok(), "open failed");
  std::vector<catalog::CatalogTypedRecord> records;
  {
    const auto page_size = created.state.header.page_size;
    std::set<platform::u64> visited;
    platform::u64 page_number = db::kCatalogPageNumber;
    while (page_number != 0) {
      Require(visited.insert(page_number).second, "catalog chain cycle");
      const auto offset = page::CheckedPageBodyOffset(page_size, page_number,
                                                      disk::kPageHeaderSerializedBytes);
      Require(offset.ok(), "catalog offset failed");
      std::vector<platform::byte> body(page_size - disk::kPageHeaderSerializedBytes, 0);
      Require(device.ReadAt(offset.offset, body.data(), body.size()).ok(), "catalog read failed");
      const auto parsed = page::ParseCatalogPageBody(body, page_number);
      Require(parsed.ok(), "catalog parse failed");
      for (const auto& row : parsed.body.rows) {
        if (row.kind != page::CatalogPageRowKind::typed_catalog_record) continue;
        const auto decoded = catalog::DecodeCatalogTypedRecord(row);
        Require(decoded.ok(), "catalog record decode failed");
        records.push_back(decoded.record);
      }
      page_number = parsed.body.next_page_number;
    }
  }
  Require(device.Close().ok(), "close failed");

  // Identities: every record's row identity, and every object identity a
  // record carries, is a binary UUIDv7; no two records share either.
  std::set<platform::Uuid> rows, objects;
  for (const auto& record : records) {
    Require(IsV7(record.header.row_uuid.value), "a record's row identity is not a UUIDv7");
    Require(rows.insert(record.header.row_uuid.value).second, "two records share a row identity");
    if (record.header.object_uuid.value.is_nil()) continue;  // sub-records name no object
    if (record.header.kind == catalog::CatalogRecordKind::localized_name ||
        record.header.kind == catalog::CatalogRecordKind::localized_comment) continue;
    Require(IsV7(record.header.object_uuid.value), "an object identity is not a UUIDv7");
    Require(objects.insert(record.header.object_uuid.value).second,
            "two records claim the same object identity");
  }

  // Named SQL objects, with the name each record itself carries.
  std::vector<Expected> expected;
  const auto simple = [](const std::string& path) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
  };
  for (const auto& record : records) {
    const auto object = record.header.object_uuid.value;
    // A security object's name, or its code when it carries no name.
    const auto security_name = [&](std::string_view key, std::string_view fallback = {}) {
      const auto security = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
      Require(security.ok(), "security record decode failed");
      auto value = security.record->Attribute(key);
      if ((!value || value->empty()) && !fallback.empty()) value = security.record->Attribute(fallback);
      return value ? std::string(*value) : std::string{};
    };
    std::string object_class, name;
    switch (record.header.kind) {
      // Filespaces are enumerated filespace_<n>; create makes the first.
      case catalog::CatalogRecordKind::filespace: object_class = "filespace"; name = "filespace_0"; break;
      case catalog::CatalogRecordKind::user_account: object_class = "user"; name = security_name("principal_name"); break;
      case catalog::CatalogRecordKind::role_account: object_class = "role"; name = security_name("role_name", "role_code"); break;
      case catalog::CatalogRecordKind::group_account: object_class = "group"; name = security_name("group_name", "group_code"); break;
      case catalog::CatalogRecordKind::table_descriptor: object_class = "table"; name = simple(Field(record.payload, "table_name")); break;
      case catalog::CatalogRecordKind::index_descriptor: object_class = "index"; name = simple(Field(record.payload, "index_name")); break;
      case catalog::CatalogRecordKind::domain: object_class = "domain"; name = Field(record.payload, "domain_name"); break;
      case catalog::CatalogRecordKind::datatype_descriptor: object_class = "datatype"; name = Field(record.payload, "canonical_name"); break;
      case catalog::CatalogRecordKind::masking_policy: object_class = "masking_policy"; name = Field(record.payload, "policy_name"); break;
      case catalog::CatalogRecordKind::rls_policy: object_class = "rls_policy"; name = Field(record.payload, "policy_name"); break;
      case catalog::CatalogRecordKind::udr_package: object_class = "udr_package"; name = Field(record.payload, "package_name"); break;
      case catalog::CatalogRecordKind::parser_package: object_class = "parser_package"; name = Field(record.payload, "package_name"); break;
      case catalog::CatalogRecordKind::sblr_module: object_class = "sblr_module"; name = Field(record.payload, "module_name"); break;
      default: continue;
    }
    Require(!name.empty(), "a named SQL object record carries no name: class " + object_class);
    expected.push_back({object, object_class, name, record.header.parent_uuid.value});
  }
  Require(expected.size() > 300, "fewer named SQL objects than create writes");

  // Through the engine's name registry: each resolves to its own UUID.
  api::EngineRequestContext context;
  context.database_path = create.path;
  context.database_uuid = api::EngineUuid{create.database_uuid.value};
  const auto loaded = api::LoadNameRegistryState(context, 0);
  if (!loaded.ok) std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  Require(loaded.ok, "the name registry did not load");
  std::map<platform::Uuid, std::vector<const api::NameRegistryEntry*>> by_object;
  for (const auto& entry : loaded.state.entries)
    by_object[platform::Uuid{entry.object_uuid.bytes}].push_back(&entry);
  std::map<std::string, std::size_t> missing;
  for (const auto& want : expected) {
    const auto found = by_object.find(want.object);
    bool ok = false;
    if (found != by_object.end())
      for (const auto* entry : found->second)
        if (entry->object_class == want.object_class && entry->language_tag == "en" &&
            (entry->display_name == want.name || entry->raw_name_text == want.name))
          ok = true;
    if (!ok) ++missing[want.object_class];
  }
  for (const auto& [object_class, count] : missing)
    std::cerr << "unresolved " << object_class << ": " << count << '\n';
  Require(missing.empty(), "named SQL objects the name registry does not resolve");

  // Every finding is reported before the gate fails, so one defect does not
  // hide the others.
  std::size_t findings = 0;
  const auto finding = [&](const std::string& message) {
    std::cerr << "FINDING " << message << '\n';
    ++findings;
  };

  // No two entries may collide on class, scope, language and lookup key.
  std::set<std::string> keys;
  for (const auto& entry : loaded.state.entries) {
    std::string key = entry.object_class + '|' + entry.language_tag + '|' + entry.normalized_lookup_key + '|';
    key.append(reinterpret_cast<const char*>(entry.scope_uuid.bytes.data()), entry.scope_uuid.bytes.size());
    if (!keys.insert(key).second)
      finding("two name entries collide: class " + entry.object_class + " name " + entry.display_name);
  }

  // Tables and indexes are scoped by an object the catalog holds.
  for (const auto& entry : loaded.state.entries) {
    if (entry.object_class != "table" && entry.object_class != "index") continue;
    const platform::Uuid scope{entry.scope_uuid.bytes};
    if (scope.is_nil() || !objects.contains(scope))
      finding("a " + entry.object_class + " name is scoped by no catalog object: " + entry.display_name);
  }

  // Forward: the name resolves back to the object, through the registry's
  // resolver, for every unscoped object (scoped tables and indexes need a
  // schema-qualified request, covered by the scope check above).
  std::size_t resolved = 0;
  for (const auto& want : expected) {
    if (want.object_class == "table" || want.object_class == "index") continue;
    api::EngineApiRequest request;
    request.context = context;
    request.option_envelopes.push_back("name:" + want.name);
    const auto found = api::ResolveNameRegistryPrivate(request, want.object_class);
    bool matched = false;
    if (found.ok)
      for (const auto& match : found.matches)
        if (platform::Uuid{match.object_uuid.bytes} == want.object) matched = true;
    if (!matched) finding("name does not resolve back: " + want.object_class + ' ' + want.name);
    else ++resolved;
  }
  Require(resolved > 300, "too few names resolved forward");

  // Character sets and collations are shared items defined in the project's
  // resource seed pack; each name there resolves, through the API the parser
  // path uses, to a record of that kind in this file.
  std::map<platform::Uuid, catalog::CatalogRecordKind> kind_of;
  for (const auto& record : records)
    if (!record.header.object_uuid.value.is_nil()) kind_of[record.header.object_uuid.value] = record.header.kind;
  const auto names_in = [&](const std::string& relative, const std::string& list_key) {
    std::ifstream in(std::filesystem::path(create.resource_seed_pack_root) / relative);
    Require(in.good(), "resource file unreadable: " + relative);
    std::stringstream text; text << in.rdbuf();
    const std::string body = text.str();
    const auto list = body.find("\"" + list_key + "\"");
    Require(list != std::string::npos, "resource list missing: " + list_key);
    std::vector<std::string> names;
    const std::regex name_field("\\{\\s*\"name\"\\s*:\\s*\"([^\"]+)\"");
    for (std::sregex_iterator it(body.begin() + static_cast<std::ptrdiff_t>(list), body.end(), name_field), end;
         it != end; ++it)
      names.push_back((*it)[1].str());
    return names;
  };
  // Resources are looked up inside a transaction, as the owner would.
  auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  api::EngineBeginTransactionRequest begin;
  begin.context = owner;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok, "owner transaction did not begin");
  owner.transaction_uuid = begun.transaction_uuid;
  owner.local_transaction_id = begun.local_transaction_id;
  owner.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  owner.transaction_isolation_level = begun.isolation_level;
  const auto resolve_resource = [&](const std::string& name, const char* resource_class,
                                    catalog::CatalogRecordKind expected_kind) {
    api::EngineResolveNameRequest request;
    request.context = owner;
    request.sql_object_reference.expected_object_type = resource_class;
    request.sql_object_reference.object_name.raw_text = name;
    const auto resolved_resource = api::EngineResolveName(request);
    const auto kind = kind_of.find(platform::Uuid{resolved_resource.primary_object.uuid.bytes});
    if (!resolved_resource.ok || kind == kind_of.end() || kind->second != expected_kind)
      finding(std::string(resource_class) + " name does not resolve to its record: " + name +
              (resolved_resource.diagnostics.empty() ? std::string{}
                   : " [" + resolved_resource.diagnostics.front().code + ':' +
                         resolved_resource.diagnostics.front().detail + ']'));
  };
  const auto charsets = names_in("resources/charsets/charsets.json", "character_sets");
  const auto collations = names_in("resources/collations/collations.json", "collations");
  Require(charsets.size() > 50 && collations.size() > 100, "resource names were not read");
  for (const auto& name : charsets) resolve_resource(name, "charset", catalog::CatalogRecordKind::charset);
  for (const auto& name : collations) resolve_resource(name, "collation", catalog::CatalogRecordKind::collation);
  {
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = owner;
    (void)api::EngineRollbackTransaction(rollback);
  }
  Require(findings == 0, std::to_string(findings) + " name registration findings");
  std::cout << "create_object_name_registration=passed objects=" << expected.size() << '\n';
  return EXIT_SUCCESS;
}
