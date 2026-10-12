#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_statement_fixture.hpp"
#include "memory.hpp"
#include "api_types.hpp"
#include "catalog/descriptor_mutation_api.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_index_visibility.hpp"
#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/insert_physical_integration.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/mutation_savepoint_capability.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "dml/update_api.hpp"
#include "extensibility/udr_api.hpp"
#include "index_family_registry.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "lifecycle/sequence_generator_lifecycle.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/show_api.hpp"
#include "query/plan_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/local_commit_publication.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kSearchKey =
    "DML_TRANSACTIONAL_INDEX_LIFECYCLE_MATRIX";

[[noreturn]] void Fail(std::string_view family, std::string_view message) {
  std::cerr << "family=" << family << ':' << message << '\n';
  throw std::runtime_error(std::string(family) + ":" + std::string(message));
}

void Require(bool condition,
             std::string_view family,
             std::string_view message) {
  if (!condition) Fail(family, message);
}

template <typename TResult>
void RequireOk(const TResult& result,
               std::string_view family,
               std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(family, message);
  }
}

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view family,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Fail(family, message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  if (!generated.ok()) Fail("fixture", "UUID creation failed");
  return generated.value;
}

api::EngineUuid NewNativeUuid(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(api::EngineUuid row_uuid,
                        std::string key,
                        std::string payload) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields.push_back({"key_value", TextValue(std::move(key))});
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

struct FamilyCase {
  const idx::IndexFamilyDescriptor* descriptor = nullptr;
  const idx::IndexFamilyPhysicalCapabilityState* capability = nullptr;
  std::string crud_family;
  bool unique = false;
  std::string old_key;
  std::string new_key;
  std::string rollback_key;
};

std::string CrudFamily(const idx::IndexFamilyDescriptor& descriptor) {
  if (descriptor.family == idx::IndexFamily::unique_btree) {
    return api::kCrudIndexFamilyBtree;
  }
  if (descriptor.family == idx::IndexFamily::graph) {
    return api::kCrudIndexFamilyGraphAdjacency;
  }
  return descriptor.id;
}

bool IsTokenFamily(idx::IndexFamily family) {
  return family == idx::IndexFamily::full_text ||
         family == idx::IndexFamily::gin ||
         family == idx::IndexFamily::inverted ||
         family == idx::IndexFamily::ngram ||
         family == idx::IndexFamily::sparse_wand ||
         family == idx::IndexFamily::document_path;
}

FamilyCase BuildCase(const idx::IndexFamilyDescriptor& descriptor) {
  FamilyCase test_case;
  test_case.descriptor = &descriptor;
  test_case.capability =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
  test_case.crud_family = CrudFamily(descriptor);
  test_case.unique = descriptor.family == idx::IndexFamily::unique_btree ||
                     descriptor.family == idx::IndexFamily::hash;
  if (descriptor.key_model == idx::IndexKeyModel::spatial_key) {
    test_case.old_key = "0,0,1,1";
    test_case.new_key = "2,2,3,3";
    test_case.rollback_key = "4,4,5,5";
  } else if (descriptor.key_model == idx::IndexKeyModel::vector_key) {
    test_case.old_key = "0,1,2,3";
    test_case.new_key = "4,5,6,7";
    test_case.rollback_key = "8,9,10,11";
  } else if (descriptor.family == idx::IndexFamily::ngram) {
    test_case.old_key = "old";
    test_case.new_key = "new";
    test_case.rollback_key = "bad";
  } else if (IsTokenFamily(descriptor.family)) {
    test_case.old_key = "alpha old";
    test_case.new_key = "bravo new";
    test_case.rollback_key = "charlie ghost";
  } else {
    test_case.old_key = "old-key";
    test_case.new_key = "new-key";
    test_case.rollback_key = "rollback-key";
  }
  return test_case;
}

api::EnginePredicateEnvelope PredicateFor(const FamilyCase& test_case,
                                          const std::string& key) {
  api::EnginePredicateEnvelope predicate;
  const auto family = test_case.descriptor->family;
  if (family == idx::IndexFamily::full_text ||
      family == idx::IndexFamily::gin ||
      family == idx::IndexFamily::inverted ||
      family == idx::IndexFamily::sparse_wand ||
      family == idx::IndexFamily::document_path) {
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(
        TextValue(key.substr(0, key.find(' '))));
  } else if (family == idx::IndexFamily::ngram) {
    predicate.predicate_kind = "text_term_contains";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (test_case.descriptor->key_model ==
             idx::IndexKeyModel::spatial_key) {
    predicate.predicate_kind = "spatial_bbox_intersects";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::vector_exact) {
    predicate.predicate_kind = "vector_exact_nearest";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::vector_hnsw ||
             family == idx::IndexFamily::vector_ivf) {
    predicate.predicate_kind = "vector_approx_nearest";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::expression) {
    predicate.predicate_kind = "expression_equals";
    predicate.canonical_predicate_envelope = "lower:key_value";
    predicate.bound_values.push_back(TextValue(key));
  } else if (family == idx::IndexFamily::partial) {
    predicate.predicate_kind = "partial_index_probe";
    predicate.canonical_predicate_envelope = "key_value=" + key;
  } else {
    predicate.predicate_kind = "column_equals";
    predicate.canonical_predicate_envelope = "key_value";
    predicate.bound_values.push_back(TextValue(key));
  }
  return predicate;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  api::EngineUuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  api::EngineUuid table_uuid;
  api::EngineUuid index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    session.reset();
    if (std::uncaught_exceptions() != 0) {
      std::cerr << "failed_family_artifacts=" << dir << '\n';
      return;
    }
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                std::string isolation = "read_committed") {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = isolation;
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "fixture", "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context, std::string_view family) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), family, "commit failed");
}

void Rollback(const api::EngineRequestContext& context,
              std::string_view family) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), family, "rollback failed");
}

api::CrudIndexRecord IndexRecord(const Fixture& fixture,
                                 const FamilyCase& test_case,
                                 const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "key_value";
  index.family = test_case.crud_family;
  index.profile = test_case.descriptor->default_semantic_profile;
  index.unique = test_case.unique;
  index.approximate = test_case.descriptor->approximate;
  index.exact_fallback = index.approximate;
  if (test_case.descriptor->family == idx::IndexFamily::expression) {
    index.key_envelopes.push_back("lower:key_value");
  } else {
    index.key_envelopes.push_back("key_value");
  }
  if (test_case.descriptor->family == idx::IndexFamily::partial) {
    index.predicate_kind = "where_true";
  }
  if (test_case.descriptor->family == idx::IndexFamily::covering) {
    index.include_columns.push_back("payload");
  }
  if (index.unique) index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(const FamilyCase& test_case, platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_tx_index_matrix_" + family + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "matrix.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), family, "database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.table_uuid = NewNativeUuid(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewNativeUuid(platform::UuidKind::object, salt + 11);

  auto context = Begin(fixture, "matrix-metadata");
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "transactional_index_matrix_" + family;
  table.columns.push_back({"key_value", "canonical=character"});
  table.columns.push_back({"payload", "canonical=character"});
  RequireDiagnosticOk(scratchbird::tests::PublishMgaTableFixture(
                          context, table, {"character", "character"},
                          {IndexRecord(fixture, test_case, context)}),
                      family, "table metadata append failed");
  RequireDiagnosticOk(
      api::AppendMgaIndexMetadata(context,
                                  IndexRecord(fixture, test_case, context)),
      family, "index metadata append failed");
  Commit(context, family);
  fixture.owner_context.current_schema_uuid = context.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "matrix-session"));
  return fixture;
}

api::EngineApiResult DispatchDml(const Fixture& fixture,
                                 const api::EngineRequestContext& context,
                                 std::string operation_id,
                                 std::string opcode,
                                 api::EngineApiRequest request,
                                 bool require_dispatch = true) {
  scratchbird::tests::FixtureEngineStatement statement(*fixture.session, context);
  auto envelope =
      sblr::MakeSblrEnvelope(operation_id, opcode, std::string(kSearchKey));
  const auto* registered = sblr::LookupSblrOperation(operation_id);
  Require(registered != nullptr && registered->opcode == opcode &&
              registered->code != 0,
          "sblr", "canonical DML opcode is not registered");
  envelope.opcode_code = registered->code;
  envelope.parser_package_uuid =
      scratchbird::tests::FixtureUuidLiteral("12340000-0000-7000-8000-000000000031");
  envelope.registry_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("12340000-0000-7000-8000-000000000032");
  envelope.requires_transaction_context = true;
  request.context = statement.context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = statement.context;
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(std::move(dispatch));
  if (require_dispatch)
    Require(result.accepted && result.envelope_validated && result.dispatched_to_api,
            "sblr", "canonical DML dispatch failed before the engine API");
  return std::move(result.api_result);
}

api::EngineApiResult Insert(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            api::EngineUuid row_uuid,
                            std::string key,
                            std::string payload) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows.push_back(
      Row(std::move(row_uuid), std::move(key), std::move(payload)));
  return DispatchDml(fixture, context, "dml.insert_rows", "SBLR_DML_INSERT_ROWS",
                     std::move(request));
}

api::EngineApiResult Update(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            api::EngineUuid row_uuid,
                            std::string key) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.row_uuid = row_uuid;
  request.assignments.push_back({"key_value", TextValue(std::move(key))});
  return DispatchDml(fixture, context, "dml.update_rows", "SBLR_DML_UPDATE_ROWS",
                     std::move(request));
}

api::EngineApiResult Delete(const Fixture& fixture,
                            const api::EngineRequestContext& context,
                            api::EngineUuid row_uuid) {
  api::EngineApiRequest request;
  request.context = context;
  request.target_object.uuid = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.row_uuid = row_uuid;
  request.option_envelopes.push_back("delete_mode:tombstone_only");
  return DispatchDml(fixture, context, "dml.delete_rows", "SBLR_DML_DELETE_ROWS",
                     std::move(request));
}

api::MgaRelationReadView LoadState(
    const api::EngineRequestContext& context,
    std::string_view family) {
  auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, family, "relation state reload failed");
  return api::BuildMgaRelationReadView(std::move(loaded.state));
}

const api::CrudIndexRecord& FindIndex(const api::MgaRelationReadView& state,
                                      const Fixture& fixture,
                                      std::string_view family) {
  for (const auto& index : state.indexes) {
    if (index.index_uuid == fixture.index_uuid) return index;
  }
  Fail(family, "durable index metadata missing");
}

bool HasMutation(const api::LocalCommitPublicationResult& publication,
                 const Fixture& fixture,
                 std::string_view kind) {
  const bool found = std::any_of(
      publication.mutations.begin(), publication.mutations.end(),
      [&](const auto& mutation) {
        return mutation.mutation_domain == "index" &&
               mutation.object_identity == fixture.index_uuid &&
               mutation.mutation_kind == kind &&
               mutation.finality_authority ==
                   "durable_transaction_inventory";
      });
  if (!found) {
    for (const auto& mutation : publication.mutations) {
      std::cerr << "manifest=" << mutation.mutation_domain << ':'
                << mutation.mutation_kind << ":identity_present=" << !mutation.object_identity.is_nil()
                << ':' << mutation.finality_authority << '\n';
    }
  }
  return found;
}

void ValidateAdmittedFamily(const FamilyCase& test_case,
                            platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  auto fixture = MakeFixture(test_case, salt);

  auto seed = Begin(fixture, "matrix-seed");
  const api::EngineUuid row_uuid =
      NewNativeUuid(platform::UuidKind::object, salt + 200);
  const auto inserted =
      Insert(fixture, seed, row_uuid, test_case.old_key, "seed");
  RequireOk(inserted, family, "insert failed");
  Require(inserted.dml_summary.rows_changed == 1, family,
          "insert did not publish one stable row identity");
  const auto seed_publication = api::RunLocalCommitPageBarrier(seed);
  Require(seed_publication.ok &&
              HasMutation(seed_publication, fixture, "insert"),
          family, "insert was absent from the publication manifest");
  Commit(seed, family);

  // Actual engine DML dispatch and native marker storage, not the generic
  // fixture SavepointStack. Exercise all admitted profiles with distinct row
  // and index cutoffs, repeated rewind, key reuse and fresh-context reopen.
  auto savepoint_writer = Begin(fixture, "matrix-savepoint-writer");
  const auto verify_savepoint_rows = [&](const api::EngineRequestContext& context,
                                         std::size_t count, std::string_view key) {
    const auto snapshot = LoadState(context, family);
    const auto rows = api::VisibleMgaRowsForContext(snapshot, fixture.table_uuid, context);
    Require(rows.size() == count, family, "savepoint visible row count differs");
    if (count == 1) {
      const auto found = std::find_if(rows[0].values.begin(), rows[0].values.end(),
          [](const auto& field) { return field.first == "key_value"; });
      Require(found != rows[0].values.end() && found->second.isPresent() && found->second.bytes == key,
              family, "savepoint restored wrong row value");
    }
    const auto& live_index = FindIndex(snapshot, fixture, family);
    const auto validated = api::MgaTransactionalIndexProvider(context, nullptr)
        .ValidateAgainstRelation(snapshot, live_index);
    Require(validated.ok, family, "savepoint index/row validation failed");
    const auto lookup = api::MgaTransactionalIndexProvider(context, nullptr)
        .ResolveVisibleEntry(snapshot, live_index,
            PredicateFor(test_case, std::string(key)), 10);
    Require(lookup.ok && lookup.rows.size() == count,
            family, "savepoint index probe returned a ghost or lost a row");
  };
  RequireDiagnosticOk(api::CreateMgaSavepointMarker(savepoint_writer, "matrix_outer"),
                      family, "savepoint creation failed");
  for (int repeat = 0; repeat != 2; ++repeat) {
    RequireOk(Update(fixture, savepoint_writer, row_uuid, test_case.new_key),
              family, "savepoint key update failed");
    verify_savepoint_rows(savepoint_writer, 1, test_case.new_key);
    RequireDiagnosticOk(api::CreateMgaSavepointMarker(savepoint_writer, "matrix_inner"),
                        family, "nested savepoint creation failed");
    RequireOk(Delete(fixture, savepoint_writer, row_uuid), family, "savepoint delete failed");
    verify_savepoint_rows(savepoint_writer, 0, test_case.new_key);
    RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(savepoint_writer, "matrix_inner"),
                        family, "nested delete rewind failed");
    verify_savepoint_rows(savepoint_writer, 1, test_case.new_key);
    RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(savepoint_writer, "matrix_outer"),
                        family, "outer update rewind failed");
    verify_savepoint_rows(savepoint_writer, 1, test_case.old_key);
    const auto extra_row = NewNativeUuid(platform::UuidKind::object, salt + 210 + repeat);
    RequireOk(Insert(fixture, savepoint_writer, extra_row, test_case.new_key, "later"),
              family, "savepoint key reuse after rewind failed");
    RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(savepoint_writer, "matrix_outer"),
                        family, "insert rewind failed");
    verify_savepoint_rows(savepoint_writer, 1, test_case.old_key);
  }
  RequireDiagnosticOk(api::ReleaseMgaSavepointMarker(savepoint_writer, "matrix_outer"),
                      family, "savepoint release failed");
  Commit(savepoint_writer, family);
  auto savepoint_reopen = Begin(fixture, "matrix-savepoint-reopen");
  verify_savepoint_rows(savepoint_reopen, 1, test_case.old_key);
  Commit(savepoint_reopen, family);

  if (test_case.unique) {
    auto duplicate = Begin(fixture, "matrix-duplicate");
    const auto rejected = Insert(
        fixture,
        duplicate,
        NewNativeUuid(platform::UuidKind::object, salt + 201),
        test_case.old_key,
        "duplicate");
    Require(!rejected.ok, family, "unique duplicate was accepted");
    Rollback(duplicate, family);
  }

  auto old_snapshot = Begin(fixture, "matrix-old-snapshot", "snapshot");
  auto updater = Begin(fixture, "matrix-update");
  const auto updated = Update(fixture, updater, row_uuid, test_case.new_key);
  RequireOk(updated, family, "key-changing update failed");
  Require(updated.dml_summary.rows_changed == 1, family,
          "key-changing update did not match one row");
  const auto update_publication = api::RunLocalCommitPageBarrier(updater);
  Require(update_publication.ok &&
              HasMutation(update_publication, fixture, "retire") &&
              HasMutation(update_publication, fixture, "insert"),
          family, "update insert/retire pair was absent from publication");
  Commit(updater, family);

  auto current_reader = Begin(fixture, "matrix-current-reader");
  const auto state = LoadState(current_reader, family);
  const auto& index = FindIndex(state, fixture, family);
  api::MgaTransactionalIndexProvider old_provider(old_snapshot, nullptr);
  api::MgaTransactionalIndexProvider current_provider(current_reader, nullptr);
  const auto old_validated = old_provider.ValidateAgainstRelation(state, index);
  const auto current_validated =
      current_provider.ValidateAgainstRelation(state, index);
  Require(old_validated.ok && old_validated.visible_entry_count != 0,
          family, "old snapshot lost pre-update index membership");
  Require(current_validated.ok && current_validated.visible_entry_count != 0,
          family, "current snapshot lost post-update index membership");
  const auto old_lookup = old_provider.ResolveVisibleEntry(
      state, index, PredicateFor(test_case, test_case.old_key), 1);
  const auto current_lookup = current_provider.ResolveVisibleEntry(
      state, index, PredicateFor(test_case, test_case.new_key), 1);
  Require(old_lookup.ok && old_lookup.rows.size() == 1,
          family, "old snapshot index lookup lost its visible row");
  Require(current_lookup.ok && current_lookup.rows.size() == 1,
          family, "current snapshot index lookup lost its visible row");
  if (test_case.descriptor->key_model != idx::IndexKeyModel::vector_key) {
    const auto retired_lookup = current_provider.ResolveVisibleEntry(
        state, index, PredicateFor(test_case, test_case.old_key), 1);
    Require(retired_lookup.ok && retired_lookup.rows.empty(),
            family, "current snapshot index lookup retained the old key");
  }
  const auto recovered =
      api::MgaTransactionalIndexProvider(updater, nullptr)
          .RecoverInterruptedMutation(state);
  Require(recovered.ok && recovered.lifecycle_state == "committed_by_inventory",
          family, "committed update recovery classification changed");
  const auto published =
      api::MgaTransactionalIndexProvider(updater, nullptr)
          .PublishTransaction(state);
  Require(published.ok && published.lifecycle_state == "published_by_inventory",
          family, "provider publication did not follow MGA inventory");
  Commit(old_snapshot, family);
  Commit(current_reader, family);

  auto rebuild = Begin(fixture, "matrix-rebuild");
  const auto rebuild_state = LoadState(rebuild, family);
  const auto& rebuild_index = FindIndex(rebuild_state, fixture, family);
  auto append = api::MgaRelationHotAppendContext(rebuild);
  api::MgaTransactionalIndexProvider rebuild_provider(rebuild, &append);
  const auto horizon_refused = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, false);
  Require(!horizon_refused.ok &&
              horizon_refused.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.REBUILD_HORIZON_REQUIRED",
          family, "rebuild did not require authoritative cleanup horizon");
  const auto rebuilt = rebuild_provider.RebuildFromRelation(
      rebuild_state, rebuild_index, true);
  Require(rebuilt.ok && rebuilt.rebuilt_entry_count != 0,
          family, "deterministic rebuild produced no entries");
  RequireDiagnosticOk(append.FlushIndexEntries(), family,
                      "rebuild index flush failed");
  Commit(rebuild, family);

  auto reopen = Begin(fixture, "matrix-reopen");
  const auto reopened_state = LoadState(reopen, family);
  const auto& reopened_index = FindIndex(reopened_state, fixture, family);
  Require(api::MgaTransactionalIndexProvider(reopen, nullptr)
              .ValidateAgainstRelation(reopened_state, reopened_index)
              .ok,
          family, "rebuilt index failed validation after durable reopen");
  Commit(reopen, family);

  auto deleter = Begin(fixture, "matrix-delete");
  const auto deleted = Delete(fixture, deleter, row_uuid);
  RequireOk(deleted, family, "delete failed");
  Require(deleted.dml_summary.rows_changed == 1, family,
          "delete did not match one row");
  const auto delete_publication = api::RunLocalCommitPageBarrier(deleter);
  Require(delete_publication.ok &&
              HasMutation(delete_publication, fixture, "retire"),
          family, "delete retirement was absent from publication");
  Commit(deleter, family);

  auto rollback_writer = Begin(fixture, "matrix-rollback-writer");
  const auto rolled_back_insert =
      Insert(fixture,
             rollback_writer,
             NewNativeUuid(platform::UuidKind::object, salt + 202),
             test_case.rollback_key,
             "rollback");
  RequireOk(rolled_back_insert, family, "rollback insert failed");
  Rollback(rollback_writer, family);

  auto final_reader = Begin(fixture, "matrix-final-reader");
  const auto final_state = LoadState(final_reader, family);
  const auto& final_index = FindIndex(final_state, fixture, family);
  const auto final_validation =
      api::MgaTransactionalIndexProvider(final_reader, nullptr)
          .ValidateAgainstRelation(final_state, final_index);
  Require(final_validation.ok && final_validation.visible_entry_count == 0,
          family, "delete or rollback left visible index membership");
  const auto final_lookup =
      api::MgaTransactionalIndexProvider(final_reader, nullptr)
          .ResolveVisibleEntry(final_state,
                               final_index,
                               PredicateFor(test_case, test_case.new_key),
                               1);
  Require(final_lookup.ok && final_lookup.rows.empty(),
          family, "delete left a visible index lookup ghost");
  for (const auto& entry : final_state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid &&
        entry.creator_tx == rollback_writer.local_transaction_id) {
      Require(!api::CrudCreatorVisible(final_state,
                                       entry.creator_tx,
                                       entry.event_sequence,
                                       final_reader.local_transaction_id),
              family, "rolled-back index entry remained MGA-visible");
    }
  }
  const auto aborted =
      api::MgaTransactionalIndexProvider(rollback_writer, nullptr)
          .AbortTransaction(final_state);
  Require(aborted.ok && aborted.lifecycle_state == "invisible_by_inventory",
          family, "rollback classification did not follow MGA inventory");
  const auto rollback_recovery =
      api::MgaTransactionalIndexProvider(rollback_writer, nullptr)
          .RecoverInterruptedMutation(final_state);
  Require(rollback_recovery.ok &&
              rollback_recovery.lifecycle_state == "abandoned_by_inventory",
          family, "rolled-back recovery classification changed");
  Commit(final_reader, family);
}

void ValidateIndexVersionProjection() {
  api::RelationReadSnapshot state;
  state.transactions = {{1, "committed"}, {2, "active"}, {3, "aborted"}};
  const auto table = NewNativeUuid(platform::UuidKind::object, 91001);
  api::CrudIndexRecord first;
  first.table_uuid = table;
  first.index_uuid = NewNativeUuid(platform::UuidKind::object, 91002);
  first.creator_tx = 1;
  first.event_sequence = 10;
  first.default_name = "first";
  first.column_name = "old_column";
  auto second = first;
  second.index_uuid = NewNativeUuid(platform::UuidKind::object, 91003);
  second.event_sequence = 11;
  auto own = first;
  own.creator_tx = 2;
  own.event_sequence = 20;
  own.default_name = "own_successor";
  own.column_name = "new_column";
  auto older = first;
  older.event_sequence = 15;
  older.default_name = "committed_successor";
  auto aborted = first;
  aborted.creator_tx = 3;
  aborted.event_sequence = 30;
  auto missing = first;
  missing.creator_tx = 4;
  missing.event_sequence = 40;
  state.indexes = {first, second, own, older, aborted, missing};
  const auto local = api::VisibleCrudIndexesForTable(state, table, 2);
  Require(local.size() == 2 && local[0].index_uuid == first.index_uuid &&
              local[0].event_sequence == 20 && local[0].default_name == own.default_name &&
              local[1].index_uuid == second.index_uuid,
          "visibility", "index winner ignored visibility, sequence or stable identity order");
  const auto sibling = api::VisibleCrudIndexesForTable(state, table, 99);
  Require(sibling.size() == 2 && sibling[0].event_sequence == 15 &&
              sibling[0].default_name == older.default_name,
          "visibility", "hidden newer index suppressed the committed predecessor");
  const auto old_key = api::VisibleCrudIndexesForTableColumn(state, table, "old_column", 2);
  Require(old_key.size() == 1 && old_key[0].index_uuid == second.index_uuid,
          "visibility", "key filtering resurrected a superseded index version");
  const auto all = api::VisibleCrudIndexes(state, 2);
  Require(all.size() == local.size() && all[0].event_sequence == 20 &&
              all[1].index_uuid == second.index_uuid,
          "visibility", "statement-wide index projection disagreed with table projection");
  auto other_table = first;
  other_table.table_uuid = NewNativeUuid(platform::UuidKind::object, 91004);
  other_table.index_uuid = NewNativeUuid(platform::UuidKind::object, 91005);
  state.indexes.push_back(other_table);
  const auto across_tables = api::VisibleCrudIndexes(state, 2);
  Require(across_tables.size() == 3 &&
              across_tables.back().index_uuid == other_table.index_uuid &&
              api::VisibleCrudIndexesForTable(state, table, 2).size() == 2 &&
              api::VisibleCrudIndexesForTable(state, other_table.table_uuid, 2).size() == 1,
          "visibility", "statement-wide projection lost or mixed another table's index");
  // Legacy ties are tested in a separate source domain, never compared to
  // native metadata sequences or treated as native reservation authority.
  state.indexes = {first, older};
  for (const auto sequence : {std::uint64_t{0}, std::uint64_t{7}}) {
    for (auto& index : state.indexes) index.event_sequence = sequence;
    const auto tied = api::VisibleCrudIndexes(state, 99);
    Require(tied.size() == 1 && tied[0].default_name == older.default_name,
            "visibility", "legacy equal-sequence projection did not retain the last visible record");
  }
}

void ValidateIndexSuccessorConsumers() {
  constexpr std::string_view family = "index-successor";
  const auto descriptor = idx::FindBuiltinIndexFamilyById("btree");
  Require(descriptor.ok(), family, "B-tree fixture family missing");
  const auto test_case = BuildCase(*descriptor.descriptor);
  auto fixture = MakeFixture(test_case, 92000);
  // The storage matrix publishes catalog identities/columns, not the schema
  // tree used by the object browser. Publish that real prerequisite for this
  // broader reader test using the same existing binary schema identity.
  auto schema_owner = Begin(fixture, "index-successor-schema-tree");
  api::EngineSchemaTreeRecord schema;
  schema.schema_uuid = schema_owner.current_schema_uuid;
  schema.default_name = "fixture_schema";
  schema.localized_names = {{"en", "primary", "", schema.default_name, true}};
  schema.payload = api::SchemaTreePayload(schema.parent_schema_uuid,
                                         schema.localized_names, {});
  RequireDiagnosticOk(api::PersistSchemaTreeRecord(schema_owner, schema,
                          "catalog.schema_tree.fixture"), family,
                      "native schema-tree prerequisite publication failed");
  auto named_index = IndexRecord(fixture, test_case, schema_owner);
  named_index.default_name = "index_successor_original";
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(schema_owner, named_index), family,
                      "named index prerequisite publication failed");
  Commit(schema_owner, family);
  auto writer = Begin(fixture, "index-successor-writer");
  const auto initial = LoadState(writer, family);
  const auto initial_indexes = api::VisibleCrudIndexesForTable(
      initial, fixture.table_uuid, writer.local_transaction_id);
  Require(initial_indexes.size() == 1 &&
              initial_indexes.front().index_uuid == fixture.index_uuid &&
              initial_indexes.front().default_name == named_index.default_name,
          family, "named committed predecessor missing");
  auto successor = initial_indexes.front();
  const auto original_name = successor.default_name;
  successor.default_name = "index_successor_own_uncommitted";
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(writer, successor), family,
                      "hidden successor publication failed");
  auto sibling = Begin(fixture, "index-successor-sibling");
  const auto verify_names = [&](const api::EngineRequestContext& context,
                                const std::string& expected_name) {
    const auto names = api::LoadNameRegistryState(context, context.local_transaction_id);
    Require(names.ok, family, "native fallback name load failed");
    std::size_t identities = 0;
    for (const auto& name : names.state.entries) {
      if (name.object_uuid != fixture.index_uuid) continue;
      ++identities;
      Require(name.derived_from_legacy_name && name.raw_name_text == expected_name,
              family, "fallback name did not select exact visible binary identity");
    }
    Require(identities == 1, family, "fallback name lost or duplicated the index");
    const auto verify_projection = [&](const api::EngineApiResult& result,
                                       const std::string& route) {
      RequireOk(result, family, "live catalog projection failed");
      std::size_t matches = 0;
      for (const auto& row : result.result_shape.rows) {
        for (const auto& [field, value] : row.fields) {
          if (field != "object_name") continue;
          if (value.encoded_value == expected_name) ++matches;
          if (value.encoded_value == original_name ||
              value.encoded_value == "index_successor_own_uncommitted" ||
              value.encoded_value == "index_successor_committed") {
            Require(value.encoded_value == expected_name, family,
                    "projection exposed a hidden or superseded index name");
          }
        }
      }
      Require(matches == 1, family, route + " projection expected exactly one " +
                  expected_name + ", found " + std::to_string(matches));
    };
    api::EngineShowCatalogRequest show;
    show.context = context;
    show.option_envelopes = {"catalog_projection:sys.catalog_readable.objects"};
    verify_projection(api::EngineShowCatalog(show), "SHOW");
    api::EnginePlanOperationRequest query;
    query.context = context;
    query.execute = true;
    query.query_operation = "values";
    query.option_envelopes = show.option_envelopes;
    verify_projection(api::EnginePlanOperation(query), "query");
  };
  verify_names(sibling, original_name);
  verify_names(writer, successor.default_name);
  Rollback(writer, family);
  verify_names(sibling, original_name);
  Commit(sibling, family);

  auto metadata = Begin(fixture, "index-successor-committed-metadata");
  successor.default_name = "index_successor_committed";
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata, successor), family,
                      "committed successor publication failed");
  Commit(metadata, family);
  auto inserter = Begin(fixture, "index-successor-insert");
  verify_names(inserter, successor.default_name);
  const auto row_uuid = NewNativeUuid(platform::UuidKind::object, 92200);
  RequireOk(Insert(fixture, inserter, row_uuid, test_case.new_key, "successor"),
            family, "DML after committed successor failed");
  const auto validation = api::ValidateTransactionalIndexMutationSetForCommit(
      inserter, LoadState(inserter, family));
  Require(validation.ok && validation.prepared_insert_count == 1 &&
              validation.prepared_retire_count == 0 && validation.visible_entry_count == 1,
          family, "commit counted historical definitions or demanded obsolete effects");
  Commit(inserter, family);
  auto reopen = Begin(fixture, "index-successor-reopen");
  const auto reopened = LoadState(reopen, family);
  const auto rows = api::VisibleMgaRowsForContext(reopened, fixture.table_uuid, reopen);
  Require(rows.size() == 1 && rows.front().row_uuid == row_uuid,
          family, "committed DML after successor did not survive reopen");
  verify_names(reopen, successor.default_name);
  Commit(reopen, family);
}

void ValidateMutationAdmissionRefusals() {
  using P = api::MgaMutationProducer;
  const auto capabilities = api::MgaMutationSavepointCapabilities();
  Require(capabilities.size() == static_cast<std::size_t>(P::count),
          "admission", "producer registry is incomplete");
  for (std::size_t n = 0; n != capabilities.size(); ++n) {
    const auto& entry = api::LookupMgaMutationSavepointCapability(static_cast<P>(n));
    Require(entry.producer == static_cast<P>(n) && !entry.name.empty() &&
                !entry.authority.empty(), "admission", "duplicate or empty producer record");
  }
  Require(api::LookupMgaMutationSavepointCapability(static_cast<P>(255)).producer == P::unknown,
          "admission", "out-of-range producer did not fail closed");
  const auto family = idx::FindBuiltinIndexFamilyById("btree");
  Require(family.ok(), "admission", "B-tree fixture family missing");
  const auto test_case = BuildCase(*family.descriptor);
  auto fixture = MakeFixture(test_case, 90000);
  auto seed = Begin(fixture, "admission-seed");
  const auto row_uuid = NewNativeUuid(platform::UuidKind::object, 90200);
  RequireOk(Insert(fixture, seed, row_uuid, test_case.old_key, "seed"),
            "admission", "admission baseline insert failed");
  api::EngineSequenceCreateGeneratorRequest create_sequence;
  create_sequence.context = seed;
  create_sequence.definition.generator_uuid = NewNativeUuid(platform::UuidKind::object, 90201);
  create_sequence.definition.database_uuid = seed.database_uuid;
  RequireOk(api::EngineSequenceCreateGenerator(create_sequence), "admission", "sequence fixture create failed");
  Commit(seed, "admission");
  // Test-only byte oracle, not transaction authority. Include every companion
  // so a refused early/direct lane cannot conceal a metadata or index write.
  const auto durable_bytes = [&] {
    std::map<std::string, std::pair<std::uint64_t, scratchbird::core::hash::Digest256>> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture.dir)) {
      if (!entry.is_regular_file()) continue;
      std::ifstream input(entry.path(), std::ios::binary);
      const auto length = std::filesystem::file_size(entry.path());
      const auto digest = scratchbird::core::hash::ComputeSha256Stream(input, length);
      Require(digest.ok() && digest.digest_bytes == scratchbird::core::hash::kSha256DigestBytes,
              "admission", "complete durable byte oracle failed");
      result.emplace(entry.path().string(), std::make_pair(length, digest.digest));
    }
    return result;
  };
  {
    auto writer = Begin(fixture, "admission-index-batch");
    const auto state = LoadState(writer, "admission");
    const auto rows = api::VisibleMgaRowsForContext(state, fixture.table_uuid, writer);
    Require(rows.size() == 1, "admission", "batch refusal baseline row missing");
    api::MgaRelationHotAppendContext append(writer);
    api::MgaTransactionalIndexProvider provider(writer, &append);
    api::DmlTransactionalIndexEntryRequest valid;
    valid.index = FindIndex(state, fixture, "admission");
    valid.table_uuid = fixture.table_uuid;
    valid.row_uuid = rows.front().row_uuid;
    valid.version_uuid = rows.front().version_uuid;
    valid.predecessor_version_uuid = rows.front().version_uuid;
    const auto valid_keys = api::CrudIndexKeysForValues(valid.index, rows.front().values);
    Require(valid_keys.size() == 1, "admission", "batch baseline logical key missing");
    valid.key_value = valid_keys.front();
    const auto before = durable_bytes();
    for (const bool retire : {false, true}) {
      for (const bool empty_key : {false, true}) {
        for (const int invalid_field : {0, 1, 2, 3, 4, 5}) {
          auto invalid = valid;
          // A present empty key is valid. It must not rescue an invalid native
          // identity, and a bad suffix must never publish the valid prefix.
          if (empty_key) {
            const auto empty_keys = api::CrudIndexKeysForValues(valid.index, {{"key_value", ""}});
            Require(empty_keys.size() == 1, "admission", "present empty logical key missing");
            invalid.key_value = empty_keys.front();
          }
          if (invalid_field == 0) invalid.version_uuid = {};
          if (invalid_field == 1) invalid.index.family = "policy_blocked";
          if (invalid_field == 2) invalid.table_uuid = {};
          if (invalid_field == 3) invalid.row_uuid = {};
          if (invalid_field == 4) invalid.index.event_sequence = 0;
          if (invalid_field == 5) invalid.version_uuid.bytes[6] =
              (invalid.version_uuid.bytes[6] & 0x0f) | 0x40;
          const auto rejected = retire ? provider.PrepareRetireEntries({valid, invalid})
                                       : provider.PrepareInsertEntries({valid, invalid});
          Require(!rejected.ok, "admission", "invalid index batch suffix was accepted");
          RequireDiagnosticOk(append.FlushIndexEntries(), "admission", "batch refusal flush failed");
          Require(durable_bytes() == before, "admission",
                  "invalid index batch suffix appended a valid prefix");
        }
      }
    }
    auto missing_predecessor = valid;
    missing_predecessor.predecessor_version_uuid = {};
    Require(!provider.PrepareRetireEntries({valid, missing_predecessor}).ok,
            "admission", "retire batch without predecessor was accepted");
    RequireDiagnosticOk(append.FlushIndexEntries(), "admission", "retire refusal flush failed");
    Require(durable_bytes() == before, "admission", "retire refusal appended a prefix");
    Rollback(writer, "admission");
  }
  {
    auto serializable = Begin(fixture, "admission-serializable", "serializable");
    RequireDiagnosticOk(api::CreateMgaSavepointMarker(serializable, "serializable_outer"),
                        "admission", "serializable boundary create failed");
    const auto before = durable_bytes();
    const auto read = api::dml::RecordSerializableSelectRead(serializable, "dml.select_rows",
                                                           fixture.table_uuid, {});
    Require(!read.ok && read.active && read.diagnostic.code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "query-invoked serializable producer bypassed boundary");
    const auto write = Insert(fixture, serializable,
        NewNativeUuid(platform::UuidKind::object, 90400), test_case.new_key, "refused-serializable");
    Require(!write.ok && !write.diagnostics.empty() &&
                write.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED" && durable_bytes() == before,
            "admission", "serializable mutation was not refused before durable writes");
    RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(serializable, "serializable_outer"),
                        "admission", "serializable refusal damaged boundary");
    RequireDiagnosticOk(api::ReleaseMgaSavepointMarker(serializable, "serializable_outer"),
                        "admission", "serializable boundary release failed");
    Require(api::dml::RecordSerializableSelectRead(serializable, "dml.select_rows",
                                                  fixture.table_uuid, {}).ok,
            "admission", "savepoint restriction leaked beyond released boundary");
    Rollback(serializable, "admission");
  }
  for (const auto& reference : {std::string("foreign_key=legacy_table:key_value"),
                                std::string("foreign_key=unresolved_reference"),
                                std::string("referenced_table_uuid=legacy_table"),
                                std::string("referenced_table_uuid=019f2100-0000-7000-8000-000000000201")}) {
    auto owner = Begin(fixture, "delete-inbound-profile");
    api::CrudTableRecord child;
    child.table_uuid = NewNativeUuid(platform::UuidKind::object, 90404);
    child.default_name = "delete_profile_child";
    child.columns = {{"key_value", "canonical=character;" + reference}};
    if (reference.starts_with("referenced_table_uuid=")) {
      const auto before = durable_bytes();
      const auto invalid = api::AppendMgaTableMetadata(owner, child);
      Require(invalid.error && invalid.detail.find("column_metadata_invalid") != std::string::npos &&
                  durable_bytes() == before,
              "admission", "text UUID column metadata reached durable storage");
      Rollback(owner, "admission");
      continue;
    }
    RequireDiagnosticOk(api::AppendMgaTableMetadata(owner, child), "admission", "inbound fixture metadata failed");
    auto state = LoadState(owner, "admission");
    const auto target = api::FindVisibleMgaTable(state, fixture.table_uuid, owner.local_transaction_id);
    Require(target.has_value(), "admission", "inbound target missing");
    const auto refused = api::ValidateDmlDeleteNoInboundConstraintProfileV1(owner, state, *target);
    Require(refused.error && refused.code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "DELETE empty-constraint profile ignored a live or unresolvable inbound reference");
    // A historical declaration must not override the exact latest visible
    // metadata version. Keep the original record in the physical history.
    child.columns = {{"key_value", "canonical=character"}};
    RequireDiagnosticOk(api::AppendMgaTableMetadata(owner, child), "admission", "child replacement metadata failed");
    state = LoadState(owner, "admission");
    RequireDiagnosticOk(api::ValidateDmlDeleteNoInboundConstraintProfileV1(owner, state, *target),
                        "admission", "retired child metadata still supplied inbound authority");
    Rollback(owner, "admission");
  }
  for (const auto& effect : {"sequence_next:019f2100-0000-7000-8000-000000000201",
                             "sblr:unregistered", "sblr_expression:unregistered",
                             "generated:unregistered"}) {
    auto metadata = Begin(fixture, "admission-default-metadata");
    const auto loaded = LoadState(metadata, "admission");
    auto table = api::FindVisibleMgaTable(loaded, fixture.table_uuid, metadata.local_transaction_id);
    Require(table.has_value(), "admission", "admission table missing");
    table->creator_tx = metadata.local_transaction_id;
    table->event_sequence = 0;
    table->columns[0].second = std::string("canonical=character;default=") + effect;
    RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, *table), "admission",
                        "default metadata persistence failed");
    Commit(metadata, "admission");
    auto writer = Begin(fixture, "admission-refused-writer");
    RequireDiagnosticOk(api::CreateMgaSavepointMarker(writer, "admission_outer"),
                        "admission", "refusal boundary creation failed");
    // Storage-only sequenced metadata has its own qualified rewind provider.
    // Its admission is independent of the still-guarded catalog/name owners.
    const auto predecessor_state = LoadState(writer, "admission");
    const auto predecessor = api::FindVisibleMgaTable(
        predecessor_state, fixture.table_uuid, writer.local_transaction_id);
    Require(predecessor.has_value(), "admission", "storage predecessor missing");
    const auto visible_index_for = [&](const api::MgaRelationReadView& state) {
      const auto indexes = api::VisibleMgaIndexesForTable(
          state, fixture.table_uuid, writer.local_transaction_id);
      const auto matches = [&](const auto& index) { return index.index_uuid == fixture.index_uuid; };
      Require(std::count_if(indexes.begin(), indexes.end(), matches) == 1,
              "admission", "exact visible index successor missing or ambiguous");
      return *std::find_if(indexes.begin(), indexes.end(), matches);
    };
    const auto predecessor_index = visible_index_for(predecessor_state);
    for (const auto suffix : {"_first_rewind", "_second_rewind"}) {
      auto changed = *predecessor;
      changed.default_name += suffix;
      RequireDiagnosticOk(api::AppendMgaTableMetadata(writer, changed),
                          "admission", "sequenced storage metadata append failed");
      auto changed_index = predecessor_index;
      changed_index.default_name += suffix;
      RequireDiagnosticOk(api::AppendMgaIndexMetadata(writer, changed_index),
                          "admission", "sequenced index metadata append failed");
      const auto changed_state = LoadState(writer, "admission");
      const auto visible_changed = api::FindVisibleMgaTable(
          changed_state, fixture.table_uuid, writer.local_transaction_id);
      Require(visible_changed.has_value() &&
                  visible_changed->default_name == changed.default_name &&
                  visible_changed->event_sequence > predecessor->event_sequence,
              "admission", "storage metadata append did not publish its successor");
      const auto visible_index = visible_index_for(changed_state);
      Require(visible_index.default_name == changed_index.default_name &&
                  visible_index.event_sequence > visible_changed->event_sequence,
              "admission", "index metadata append did not publish its sequenced successor");
      RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(writer, "admission_outer"),
                          "admission", "storage metadata rollback failed");
      const auto rewound_state = LoadState(writer, "admission");
      const auto rewound = api::FindVisibleMgaTable(
          rewound_state, fixture.table_uuid, writer.local_transaction_id);
      Require(rewound.has_value() && rewound->default_name == predecessor->default_name &&
                  rewound->columns == predecessor->columns &&
                  rewound->event_sequence == predecessor->event_sequence,
              "admission", "repeated storage metadata rollback did not restore exact predecessor");
      const auto rewound_index = visible_index_for(rewound_state);
      Require(rewound_index.default_name == predecessor_index.default_name &&
                  rewound_index.event_sequence == predecessor_index.event_sequence &&
                  rewound_index.table_uuid == predecessor_index.table_uuid &&
                  rewound_index.key_envelopes == predecessor_index.key_envelopes &&
                  rewound_index.include_columns == predecessor_index.include_columns,
              "admission", "repeated index metadata rollback did not restore predecessor");
    }
    const auto before = durable_bytes();
    for (const auto& capability : capabilities) {
      const auto observed = api::AdmitMgaSavepointProducer(writer, capability.producer);
      const bool denied = capability.disposition == api::MgaSavepointDisposition::unsupported ||
                          capability.disposition == api::MgaSavepointDisposition::prohibited;
      Require(observed.error == denied && (!denied || observed.code == "SBLR.OPERATION_UNSUPPORTED"),
              "admission", "lower producer boundary disagrees with capability registry");
    }
    api::EngineSequenceAllocateValueRequest sequence;
    sequence.context = writer;
    sequence.generator_uuid = create_sequence.definition.generator_uuid;
    const auto sequence_refused = api::EngineSequenceAllocateValue(sequence);
    Require(!sequence_refused.ok && !sequence_refused.diagnostics.empty() &&
                sequence_refused.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "direct sequence producer bypassed savepoint admission");
    const std::vector<api::EngineRowValue> direct_rows{
        Row(NewNativeUuid(platform::UuidKind::object, 90402), test_case.new_key, "direct-refused")};
    api::dml::DirectPhysicalBulkAppendRequest direct;
    direct.context = writer;
    direct.target_table.uuid = fixture.table_uuid;
    direct.borrowed_input_rows = direct_rows;
    direct.direct_lane_enabled = true;
    const auto direct_refused = api::dml::ExecuteDirectPhysicalBulkAppend(direct);
    Require(!direct_refused.ok && !direct_refused.diagnostics.empty() &&
                direct_refused.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "direct bulk entry bypassed complete-statement provider admission");
    api::EngineInvokeUdrPackageRequest udr;
    udr.context = writer;
    const auto udr_refused = api::EngineInvokeUdrPackage(udr);
    Require(!udr_refused.ok && !udr_refused.diagnostics.empty() &&
                udr_refused.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "unproven native UDR invocation bypassed savepoint admission");
    api::EngineCatalogDescriptorMutationRequest catalog;
    catalog.context = writer;
    catalog.operation_id = "catalog.mutation.descriptor";
    catalog.target_object.uuid = fixture.table_uuid;
    const auto catalog_refused = api::EngineCatalogDescriptorMutation(catalog);
    Require(!catalog_refused.ok && !catalog_refused.diagnostics.empty() &&
                catalog_refused.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "direct catalog producer bypassed savepoint admission");
    api::MgaRelationStorageDescriptor sealed_descriptor;
    const auto sealed_refused = api::AppendMgaTableMetadataWithSealedContextualTextDescriptorV2(
        writer, *table, {}, &sealed_descriptor);
    Require(sealed_refused.error && sealed_refused.code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "sealed table metadata append bypassed savepoint admission");
    const auto constraint_refused = api::AppendMgaConstraintMutationBatch(writer, {});
    Require(constraint_refused.error && constraint_refused.code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "constraint catalog batch bypassed savepoint admission");
    {
      api::MgaIndexEntryAppendBatch admitted_batch;
      admitted_batch.index = IndexRecord(fixture, test_case, writer);
      admitted_batch.table_uuid = fixture.table_uuid;
      const auto version = NewNativeUuid(platform::UuidKind::row, 90403);
      admitted_batch.rows.push_back({row_uuid, version, {{"key_value", test_case.new_key}}});
      auto unregistered = admitted_batch;
      unregistered.index.family = "unregistered_savepoint_test_family";
      unregistered.index.profile = "unregistered_savepoint_test_profile";
      api::MgaRelationHotAppendContext append(writer);
      const auto refused = append.AppendIndexEntryBatches({admitted_batch, unregistered});
      Require(refused.error && refused.code == "SBLR.OPERATION_UNSUPPORTED" &&
                  append.counters().index_materialization_jobs_queued == 0 &&
                  !append.FlushIndexEntries().error && durable_bytes() == before,
              "admission", "index batch queued a prefix before refusing an unknown producer");
      api::MgaExactIndexEntryAppendBatch exact;
      exact.index = admitted_batch.index; exact.table_uuid = fixture.table_uuid;
      exact.entries.push_back({"test-key", "test-payload", row_uuid, version});
      auto unknown_exact = exact;
      unknown_exact.index = unregistered.index;
      api::MgaRelationHotAppendContext exact_append(writer);
      const auto exact_refused = exact_append.AppendExactIndexEntryBatches({exact, unknown_exact});
      Require(exact_refused.error && exact_refused.code == "SBLR.OPERATION_UNSUPPORTED" &&
                  exact_append.counters().index_range_reservations == 0 &&
                  exact_append.counters().index_materialization_jobs_queued == 0 &&
                  !exact_append.FlushIndexEntries().error && durable_bytes() == before,
              "admission", "exact index batch wrote a prefix before refusing an unknown producer");
    }
    scratchbird::tests::FixtureEngineRequest<api::EngineSecurityCreateRoleRequest> security(
        *fixture.session, writer);
    security.role_uuid = NewNativeUuid(platform::UuidKind::object, 90401);
    security.role_name = "savepoint_refused_role";
    const auto security_refused = api::EngineSecurityCreateRole(security);
    Require(!security_refused.ok && !security_refused.diagnostics.empty() &&
                security_refused.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "direct security producer bypassed savepoint admission");
    std::set<api::EngineUuid> reclaimed;
    std::uint64_t reclaimed_count = 0;
    api::CrudRowVersionRecord reclaim_row;
    reclaim_row.table_uuid = fixture.table_uuid; reclaim_row.row_uuid = row_uuid;
    const auto reclaim_refused = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
        writer, writer.local_transaction_id, reclaim_row, "savepoint-test", &reclaimed, &reclaimed_count);
    Require(reclaim_refused.error && reclaim_refused.code == "SBLR.OPERATION_UNSUPPORTED" &&
                reclaimed.empty() && reclaimed_count == 0 && durable_bytes() == before,
            "admission", "direct reclamation or another rejected producer changed state");
    const auto rejected = Insert(fixture, writer,
        NewNativeUuid(platform::UuidKind::object, 90300), test_case.new_key, "refused");
    Require(!rejected.ok && !rejected.diagnostics.empty() &&
                rejected.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED",
            "admission", "unproven default provider was not refused by engine DML");
    Require(durable_bytes() == before, "admission", "refused mutation changed durable bytes");
    auto cancelled = writer;
    cancelled.query_cancellation_requested = [] { return true; };
    const auto cancellation = api::AdmitMgaDmlSavepointMutation(cancelled, fixture.table_uuid,
        api::MgaDmlMutationKind::update);
    Require(cancellation.error && cancellation.code == "PROCESS.CANCELLED" &&
                durable_bytes() == before, "admission", "cancelled admission changed state");
    const auto unknown = api::AdmitMgaDmlSavepointMutation(writer, fixture.table_uuid,
        static_cast<api::MgaDmlMutationKind>(255));
    Require(unknown.error && durable_bytes() == before, "admission", "unknown mutation was admitted");
    for (const auto operation : {"engine.op.sequence_nextval", "dml.execute_import_rows",
         "dml.execute_native_bulk_ingest", "engine.op.kv_structured_mutate",
         "engine.op.ddl_create_table", "future.unregistered_mutation"}) {
      const auto blocked = api::AdmitMgaSavepointOperation(writer, operation);
      Require(blocked.error && blocked.code == "SBLR.OPERATION_UNSUPPORTED",
              "admission", "unregistered canonical effect bypassed the active boundary");
    }
    const auto ddl_blocked = DispatchDml(fixture, writer, "ddl.create_table", "SBLR_DDL_CREATE_TABLE", {}, false);
    Require(!ddl_blocked.ok && !ddl_blocked.diagnostics.empty() &&
                ddl_blocked.diagnostics.front().code == "SBLR.OPERATION_UNSUPPORTED" &&
                durable_bytes() == before,
            "admission", "dispatcher did not refuse unproven catalog effects before mutation");
    RequireOk(Update(fixture, writer, row_uuid, test_case.new_key), "admission",
              "refusal invalidated the outer transaction");
    RequireDiagnosticOk(api::RollbackToMgaSavepointMarker(writer, "admission_outer"),
                        "admission", "refusal invalidated the outer boundary");
    const auto final_state = LoadState(writer, "admission");
    const auto final_rows = api::VisibleMgaRowsForContext(final_state, fixture.table_uuid, writer);
    Require(final_rows.size() == 1 && final_rows[0].row_uuid == row_uuid,
            "admission", "refusal/rewind changed original rows");
    Require(api::MgaTransactionalIndexProvider(writer, nullptr)
        .ValidateAgainstRelation(final_state, FindIndex(final_state, fixture, "admission")).ok,
        "admission", "subsequent update/rewind damaged index membership");
    RequireDiagnosticOk(api::ReleaseMgaSavepointMarker(writer, "admission_outer"),
                        "admission", "outer boundary release failed");
    Commit(writer, "admission");
  }
}

void ValidateNonAdmittedFamily(const FamilyCase& test_case,
                               platform::u64 salt) {
  const std::string& family = test_case.descriptor->id;
  api::CrudIndexRecord index;
  index.index_uuid = scratchbird::tests::FixtureUuid(1274, 401);
  index.table_uuid = scratchbird::tests::FixtureUuid(1419, 1);
  index.family = test_case.crud_family;
  index.profile = test_case.descriptor->default_semantic_profile;
  index.key_envelopes.push_back("key_value");
  Require(!api::IsAdmittedMgaTransactionalIndexFamily(index), family,
          "non-admitted family was exposed by the transactional provider");
  api::EngineRequestContext context;
  context.local_transaction_id = 1;
  context.transaction_uuid = scratchbird::tests::FixtureUuid(1208, 2701);
  api::MgaTransactionalIndexProvider provider(context, nullptr);
  api::DmlTransactionalIndexEntryRequest request;
  request.index = index;
  request.table_uuid = index.table_uuid;
  request.row_uuid = scratchbird::tests::FixtureUuid(1419, 2);
  request.version_uuid = scratchbird::tests::FixtureUuid(1419, 3);
  request.key_value = "key";
  const auto refused = provider.PrepareInsertEntry(request);
  Require(!refused.ok &&
              refused.diagnostic.code ==
                  "INDEX.TRANSACTIONAL_PROVIDER.FAMILY_NOT_ADMITTED",
          family, "non-admitted provider did not fail closed");

  auto fixture = MakeFixture(test_case, salt);
  auto writer = Begin(fixture, "matrix-non-admitted-insert");
  const auto dml_refused =
      Insert(fixture,
             writer,
             NewNativeUuid(platform::UuidKind::object, salt + 203),
             test_case.old_key,
             "must-not-publish");
  Require(!dml_refused.ok, family,
          "ordinary DML silently accepted a non-admitted index family");
  Rollback(writer, family);
  auto reader = Begin(fixture, "matrix-non-admitted-reader");
  const auto state = LoadState(reader, family);
  Require(VisibleCrudRowsForContext(state,
                                    fixture.table_uuid,
                                    reader).empty(),
          family, "refused non-admitted DML left a visible row");
  Commit(reader, family);
}

}  // namespace

int main(int argc, char** argv) {
  const bool admission_only = argc == 2 && std::string_view(argv[1]) == "--admission-only";
  if (argc != 1 && !admission_only) {
    std::cerr << "expected no arguments or --admission-only\n";
    return EXIT_FAILURE;
  }
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "transactional_index_matrix_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "transactional_index_matrix_fixture").ok(),
          "matrix", "memory policy configuration failed");
  Require(kSearchKey == "DML_TRANSACTIONAL_INDEX_LIFECYCLE_MATRIX",
          "matrix", "search key drifted");
  std::size_t admitted = 0;
  std::size_t admitted_btree_profiles = 0;
  std::size_t admitted_non_btree_profiles = 0;
  std::size_t refused = 0;
  std::size_t failures = 0;
  try {
    ValidateIndexVersionProjection();
    ValidateIndexSuccessorConsumers();
    ValidateMutationAdmissionRefusals();
  } catch (const std::exception& error) {
    ++failures;
    std::cerr << "mutation_admission=failed " << error.what() << '\n';
  }
  if (admission_only) {
    if (failures == 0) std::cout << "mutation_admission=passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  platform::u64 salt = 10000;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    try {
    const auto test_case = BuildCase(descriptor);
    Require(test_case.capability != nullptr, descriptor.id,
            "capability record missing from registry");
    api::CrudIndexRecord probe;
    probe.family = test_case.crud_family;
    probe.profile = descriptor.default_semantic_profile;
    probe.unique = test_case.unique;
    Require(api::IsAdmittedMgaSavepointIndexProfile(probe) ==
                api::IsAdmittedMgaTransactionalIndexFamily(probe), descriptor.id,
            "savepoint registry omitted or added a released default profile");
    auto unknown_profile = probe;
    unknown_profile.profile = "future_unregistered_profile";
    Require(!api::IsAdmittedMgaSavepointIndexProfile(unknown_profile), descriptor.id,
            "unknown profile inherited family savepoint capability");
    if (api::IsAdmittedMgaTransactionalIndexFamily(probe)) {
      Require(test_case.capability->runtime_available, descriptor.id,
              "provider admitted a registry-unavailable family");
      ValidateAdmittedFamily(test_case, salt);
      if (descriptor.native_physical_family == "btree") {
        ++admitted_btree_profiles;
      } else {
        ++admitted_non_btree_profiles;
      }
      ++admitted;
      std::cout << "profile=" << descriptor.id
                << " physical_family=" << descriptor.native_physical_family
                << " admission=admitted lifecycle=passed\n";
    } else {
      ValidateNonAdmittedFamily(test_case, salt);
      ++refused;
      std::cout << "profile=" << descriptor.id
                << " physical_family=" << descriptor.native_physical_family
                << " admission=refused lifecycle=fail_closed\n";
    }
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "profile=" << descriptor.id << " lifecycle=failed " << error.what() << '\n';
    }
    salt += 1000;
  }
  if (failures != 0) {
    std::cerr << "transactional_index_lifecycle_matrix failures=" << failures << '\n';
    return EXIT_FAILURE;
  }
  Require(admitted != 0, "matrix", "no native family was admitted");
  Require(admitted_btree_profiles != 0, "matrix",
          "no B-tree profile was covered");
  Require(admitted_non_btree_profiles != 0, "matrix",
          "no non-B-tree profile was covered");
  Require(admitted + refused == idx::BuiltinIndexFamilyDescriptors().size(),
          "matrix", "registry family was omitted from lifecycle matrix");
  std::cout << "transactional_index_lifecycle_matrix admitted=" << admitted
            << " btree_profiles=" << admitted_btree_profiles
            << " non_btree_profiles=" << admitted_non_btree_profiles
            << " refused=" << refused
            << " total=" << (admitted + refused) << '\n';
  return EXIT_SUCCESS;
}
