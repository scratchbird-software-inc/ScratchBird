// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "index_family_registry.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR-P4-02 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct FamilyCase {
  std::string label;
  std::string family;
  std::string profile;
  idx::IndexFamily advertised_family = idx::IndexFamily::unknown;
  std::string column_name;
  std::vector<std::string> key_envelopes;
  std::vector<std::string> include_columns;
  std::string predicate_kind;
  std::string predicate_column;
  std::string predicate_value;
  bool unique = false;
  bool approximate = false;
};

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  platform::u64 salt = 0;
  api::EngineRequestContext context;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string prefix, int ordinal) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(prefix + "-id-" + std::to_string(ordinal))});
  row.fields.push_back({"name", TextValue(prefix + "-Name-" + std::to_string(ordinal))});
  row.fields.push_back(
      {"note",
       TextValue("alpha beta gamma " + prefix + " " + std::to_string(ordinal))});
  row.fields.push_back({"tag", TextValue("hot")});
  row.fields.push_back({"bbox", TextValue("0,0,10,10")});
  row.fields.push_back({"vec", TextValue("[0.1,0.2,0.3]")});
  row.fields.push_back({"ts", TextValue("2026-06-18T00:00:0" + std::to_string(ordinal) + "Z")});
  row.fields.push_back({"doc", TextValue("$.tenant.items[" + std::to_string(ordinal) + "].name")});
  row.fields.push_back({"from_node", TextValue("node-" + std::to_string(ordinal))});
  row.fields.push_back({"to_node", TextValue("node-" + std::to_string(ordinal + 1))});
  return row;
}

std::vector<api::EngineRowValue> Rows(const std::string& label) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(3);
  for (int index = 0; index < 3; ++index) {
    rows.push_back(Row(label, index + 1));
  }
  return rows;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_index_family_preallocation";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
  table.columns.push_back({"bbox", "canonical=character"});
  table.columns.push_back({"vec", "canonical=character"});
  table.columns.push_back({"ts", "canonical=character"});
  table.columns.push_back({"doc", "canonical=character"});
  table.columns.push_back({"from_node", "canonical=character"});
  table.columns.push_back({"to_node", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture, const FamilyCase& family) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = family.column_name;
  index.family = family.family;
  index.profile = family.profile;
  index.default_name = "ipar_index_family_preallocation_" + family.label;
  index.key_envelopes = family.key_envelopes;
  index.include_columns = family.include_columns;
  index.predicate_kind = family.predicate_kind;
  index.predicate_column = family.predicate_column;
  index.predicate_value = family.predicate_value;
  index.unique = family.unique;
  index.approximate = family.approximate;
  return index;
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P4-02 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

Fixture MakeFixture(const FamilyCase& family, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_index_family_prealloc_" +
                 family.label + "_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_index_family_preallocation.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  create.page_size = 4096;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR-P4-02 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "ipar-p4-02-metadata-" + family.label);

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "IPAR-P4-02 table metadata append failed");
  const auto index = Index(fixture, family);
  const auto metadata = api::AppendMgaIndexMetadata(fixture.context, index);
  Require(!metadata.error, "IPAR-P4-02 index metadata append failed");
  Require(!api::CrudIndexKeysForValues(index, api::RowValuePairs(Row(family.label, 1))).empty(),
          "IPAR-P4-02 index family produced no keys for the proof row");
  return fixture;
}

std::vector<std::string> DemandOptions() {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=8",
          "dml_demand_hints.available_capacity_pages=8"};
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

api::EngineApiU64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) {
      continue;
    }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(item.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

void RequireIndexPreallocationEvidence(
    const FamilyCase& family,
    const std::vector<api::EngineEvidenceReference>& evidence) {
  Require(HasEvidence(evidence, "dml_demand_hint_decision", "accepted"),
          "IPAR-P4-02 demand hint was not accepted");
  Require(HasEvidence(evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "IPAR-P4-02 filespace agent did not approve demand");
  Require(HasEvidence(evidence, "page_agent_demand_decision",
                      "preallocation_completed"),
          "IPAR-P4-02 page agent did not preallocate");
  Require(HasEvidence(evidence, "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "IPAR-P4-02 index allocation did not hit preallocated pool");
  Require(HasEvidence(evidence, "index_page_preallocation_claim",
                      "physical_preallocated_pages"),
          "IPAR-P4-02 index pages were not physically preallocated");
  Require(EvidenceU64(evidence, "index_page_preallocation_granted_pages") > 0,
          "IPAR-P4-02 index granted page count missing");
  Require(!family.label.empty(), "IPAR-P4-02 family label missing");
}

api::EngineInsertRowsRequest InsertRequest(Fixture& fixture,
                                           const FamilyCase& family,
                                           std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "ipar-p4-02-insert-" + family.label;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = DemandOptions();
  return request;
}

std::vector<FamilyCase> FamilyCases() {
  return {
      {"btree",
       api::kCrudIndexFamilyBtree,
       api::kCrudIndexProfileRowStoreScalarBtreeV1,
       idx::IndexFamily::btree,
       "name",
       {"name"}},
      {"unique_btree",
       api::kCrudIndexFamilyBtree,
       "btree_unique",
       idx::IndexFamily::unique_btree,
       "id",
       {"id", "unique"},
       {},
       {},
       {},
       {},
       true},
      {"expression",
       api::kCrudIndexFamilyExpression,
       "expression",
       idx::IndexFamily::expression,
       "lower:name",
       {"lower:name"}},
      {"partial",
       api::kCrudIndexFamilyPartial,
       "partial",
       idx::IndexFamily::partial,
       "name",
       {"name"},
       {},
       "where_eq",
       "tag",
       "hot"},
      {"covering",
       api::kCrudIndexFamilyCovering,
       "covering",
       idx::IndexFamily::covering,
       "name",
       {"name"},
       {"note"}},
      {"hash",
       api::kCrudIndexFamilyHash,
       "hash",
       idx::IndexFamily::hash,
       "name",
       {"name"}},
      {"bitmap",
       api::kCrudIndexFamilyBitmap,
       "native_bitmap",
       idx::IndexFamily::bitmap,
       "tag",
       {"tag"}},
      {"brin_zone",
       api::kCrudIndexFamilyBrinZone,
       "native_zone_summary",
       idx::IndexFamily::brin_zone,
       "ts",
       {"ts"}},
      {"bloom",
       api::kCrudIndexFamilyBloom,
       "native_bloom_summary",
       idx::IndexFamily::bloom,
       "name",
       {"name"}},
      {"full_text",
       api::kCrudIndexFamilyFullText,
       "native_full_text",
       idx::IndexFamily::full_text,
       "note",
       {"note"}},
      {"gin",
       api::kCrudIndexFamilyGin,
       "postgresql_gin_profile",
       idx::IndexFamily::gin,
       "note",
       {"note"}},
      {"inverted",
       api::kCrudIndexFamilyInverted,
       "native_inverted",
       idx::IndexFamily::inverted,
       "note",
       {"note"}},
      {"ngram",
       api::kCrudIndexFamilyNgram,
       "native_ngram",
       idx::IndexFamily::ngram,
       "note",
       {"note"}},
      {"sparse_wand",
       api::kCrudIndexFamilySparseWand,
       "native_sparse_wand",
       idx::IndexFamily::sparse_wand,
       "note",
       {"note"}},
      {"spatial",
       api::kCrudIndexFamilySpatial,
       "native_spatial",
       idx::IndexFamily::spatial,
       "bbox",
       {"bbox"}},
      {"rtree",
       api::kCrudIndexFamilyRtree,
       "native_rtree",
       idx::IndexFamily::rtree,
       "bbox",
       {"bbox"}},
      {"gist",
       api::kCrudIndexFamilyGist,
       "postgresql_gist_profile",
       idx::IndexFamily::gist,
       "bbox",
       {"bbox"}},
      {"spgist",
       api::kCrudIndexFamilySpgist,
       "postgresql_spgist_profile",
       idx::IndexFamily::spgist,
       "bbox",
       {"bbox"}},
      {"vector_exact",
       api::kCrudIndexFamilyVectorExact,
       "native_vector_exact",
       idx::IndexFamily::vector_exact,
       "vec",
       {"vec"}},
      {"vector_hnsw",
       api::kCrudIndexFamilyVectorHnsw,
       "vector_hnsw",
       idx::IndexFamily::vector_hnsw,
       "vec",
       {"vec"},
       {},
       {},
       {},
       {},
       false,
       true},
      {"vector_ivf",
       api::kCrudIndexFamilyVectorIvf,
       "vector_ivf",
       idx::IndexFamily::vector_ivf,
       "vec",
       {"vec"},
       {},
       {},
       {},
       {},
       false,
       true},
      {"columnar_zone",
       api::kCrudIndexFamilyColumnarZone,
       "native_columnar_zone",
       idx::IndexFamily::columnar_zone,
       "ts",
       {"ts"}},
      {"document_path",
       api::kCrudIndexFamilyDocumentPath,
       "native_document_path",
       idx::IndexFamily::document_path,
       "doc",
       {"doc"}},
      {"graph",
       api::kCrudIndexFamilyGraphAdjacency,
       "native_graph_lookup",
       idx::IndexFamily::graph,
       "from_node",
       {"from_node"}},
      {"temporary_work",
       api::kCrudIndexFamilyTemporaryWork,
       "native_temporary_work",
       idx::IndexFamily::temporary_work,
       "name",
       {"name"}},
      {"in_memory",
       api::kCrudIndexFamilyInMemory,
       "native_in_memory",
       idx::IndexFamily::in_memory,
       "name",
       {"name"}},
  };
}

void VerifyFamily(const FamilyCase& family, platform::u64 salt) {
  Require(api::CrudIndexFamilyForProfile(family.profile) == family.family,
          "IPAR-P4-02 CRUD profile does not resolve to the expected family");
  auto fixture = MakeFixture(family, salt);
  const auto inserted = api::EngineInsertRows(
      InsertRequest(fixture, family, Rows(family.label)));
  RequireOk(inserted, "IPAR-P4-02 insert preallocation failed");
  Require(inserted.inserted_count == 3, "IPAR-P4-02 insert row count mismatch");
  RequireIndexPreallocationEvidence(family, inserted.evidence);
  Require(inserted.dml_summary.preallocation_granted_pages > 0,
          "IPAR-P4-02 summary missing preallocation grants");
  std::cout << "IPAR-P4-02 family preallocation proved: " << family.label << '\n';
}

void AssertNoAdvertisedUnsupportedCompleteFamilies(
    const std::set<idx::IndexFamily>& exercised) {
  std::vector<std::string> unsupported;
  for (const auto& state : idx::BuiltinIndexFamilyPhysicalCapabilityStates()) {
    if (!state.declared_capability || !state.runtime_available ||
        !state.physically_complete()) {
      continue;
    }
    if (exercised.count(state.family) != 0) {
      continue;
    }
    const auto* descriptor = idx::FindBuiltinIndexFamily(state.family);
    const std::string id = descriptor != nullptr ? descriptor->id
                                                 : idx::IndexFamilyName(state.family);
    const std::string native = descriptor != nullptr ? descriptor->native_physical_family
                                                     : "unknown";
    const std::string profile = descriptor != nullptr ? descriptor->default_semantic_profile
                                                      : "unknown";
    unsupported.push_back(id + " native=" + native + " profile=" + profile);
  }
  if (!unsupported.empty()) {
    std::cerr << "IPAR-P4-02 advertised complete index families without an "
                 "exercisable CRUD preallocation route:\n";
    for (const auto& family : unsupported) {
      std::cerr << "  " << family << '\n';
    }
    Fail("IPAR-P4-02 advertised index family lacks exercisable preallocation proof");
  }
}

}  // namespace

int main() {
  const platform::u64 salt = TimeSeed();
  std::set<idx::IndexFamily> exercised;
  const auto families = FamilyCases();
  for (std::size_t index = 0; index < families.size(); ++index) {
    VerifyFamily(families[index], salt + static_cast<platform::u64>(index * 10000));
    exercised.insert(families[index].advertised_family);
  }
  AssertNoAdvertisedUnsupportedCompleteFamilies(exercised);
  return EXIT_SUCCESS;
}
