// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/structured_type_api.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "sblr_admission.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include "canonical_sblr_admission_test_helper.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace server = scratchbird::server;
namespace sblr = scratchbird::engine::sblr;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kPrincipalUuid =
    "019f0600-0000-7000-8000-000000000001";
constexpr std::string_view kSchemaUuid =
    "019f0600-0000-7000-8000-000000000002";
constexpr std::string_view kCompositeUuid =
    "019f0600-0000-7000-8000-000000000101";
constexpr std::string_view kCompositeShorthandUuid =
    "019f0600-0000-7000-8000-000000000102";
constexpr std::string_view kEnumUuid =
    "019f0600-0000-7000-8000-000000000103";
constexpr std::string_view kRangeUuid =
    "019f0600-0000-7000-8000-000000000104";
constexpr std::string_view kNativeRangeUuid =
    "019f0600-0000-7000-8000-000000000105";
constexpr std::string_view kMultirangeUuid =
    "019f0600-0000-7000-8000-000000000106";
constexpr std::string_view kVariantUuid =
    "019f0600-0000-7000-8000-000000000107";
constexpr std::string_view kSetUuid =
    "019f0600-0000-7000-8000-000000000108";

struct Fixture {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  txn::LocalTransactionId typed_local_transaction_id;
  txn::LocalTransactionInventory inventory;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".sb.api_events",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) return {};
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

bool AnyFieldValue(const api::EngineApiResult& result,
                   std::string_view field,
                   std::string_view expected) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    if (FieldValue(result, field, i) == expected) return true;
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "canonical";
  localized.path = "users.public.structured_types";
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

void Grant(api::EngineRequestContext* context,
           std::string right,
           std::string target_uuid = "*") {
  api::EngineAuthorizationSubject subject;
  subject.subject_uuid.canonical = std::string(kPrincipalUuid);
  subject.subject_kind = "user";
  context->authorization_context.effective_subjects.push_back(subject);

  api::EngineMaterializedAuthorizationGrant grant;
  grant.subject_uuid.canonical = std::string(kPrincipalUuid);
  grant.subject_kind = "user";
  grant.target_uuid.canonical = std::move(target_uuid);
  grant.right = std::move(right);
  context->authorization_context.grants.push_back(std::move(grant));
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::vector<std::string> rights) {
  api::EngineRequestContext context;
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.session_uuid.canonical = "019f0600-0000-7000-8000-000000000010";
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = fixture.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      fixture.local_transaction_id;
  context.security_context_present = true;
  context.authorization_context.present = true;
  context.authorization_context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.authorization_context.authority_uuid.canonical =
      "019f0600-0000-7000-8000-000000000020";
  context.authorization_context.evidence_tags.push_back("structured_type_conformance");
  for (auto& right : rights) Grant(&context, std::move(right));
  return context;
}

Fixture CreateFixture() {
  Fixture fixture;
  fixture.path = std::filesystem::temp_directory_path() /
                 "scratchbird_sbsql_miss006_structured_type.sbdb";
  Cleanup(fixture.path);

  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1790600000000);
  Require(database_uuid.ok(), "database UUID generation failed");
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1790600000001);
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790600000002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  fixture.inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790600000003);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = txn::BeginLocalTransaction(std::move(fixture.inventory),
                                          transaction_uuid.value,
                                          1790600000004);
  Require(begun.ok(), "local transaction begin failed");
  fixture.inventory = std::move(begun.inventory);
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  fixture.local_transaction_id = begun.entry.identity.local_id.value;
  fixture.typed_local_transaction_id = begun.entry.identity.local_id;
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture.path.string(),
                                                         fixture.inventory)
              .ok(),
          "active transaction inventory persist failed");
  return fixture;
}

void CommitFixture(Fixture* fixture) {
  auto committed = txn::CommitLocalTransaction(std::move(fixture->inventory),
                                               fixture->typed_local_transaction_id,
                                               1790600000100);
  Require(committed.ok(), "local transaction commit failed");
  const auto reopen_transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790600000101);
  Require(reopen_transaction_uuid.ok(), "reopen transaction UUID generation failed");
  auto reopened = txn::BeginLocalTransaction(std::move(committed.inventory),
                                             reopen_transaction_uuid.value,
                                             1790600000102);
  Require(reopened.ok(), "reopen transaction begin failed");
  fixture->inventory = std::move(reopened.inventory);
  fixture->transaction_uuid =
      uuid::UuidToString(reopen_transaction_uuid.value.value);
  fixture->local_transaction_id = reopened.entry.identity.local_id.value;
  fixture->typed_local_transaction_id = reopened.entry.identity.local_id;
  Require(db::PersistLocalTransactionInventoryToDatabase(fixture->path.string(),
                                                         fixture->inventory)
              .ok(),
          "reopen transaction inventory persist failed");
}

void ValidateAdmissionAndRegistry() {
  struct Route {
    std::string retired_operation_id;
    std::string operation_id;
    std::string opcode;
    std::string family;
    std::string operand_contract;
    std::string result_contract;
    bool requires_txn;
  };
  const Route routes[] = {
      {"catalog.mutation.create_type", "engine.op.ddl_create_type",
       "SBLR_DDL_CREATE_TYPE", "sblr.catalog.mutation.v3",
       "create_type_descriptor", "ddl_result", true},
      {"catalog.mutation.alter_type", "engine.op.ddl_alter_type",
       "SBLR_DDL_ALTER_TYPE", "sblr.catalog.mutation.v3",
       "alter_type_descriptor", "ddl_result", true},
      {"catalog.mutation.drop_type", "engine.op.ddl_drop_type",
       "SBLR_DDL_DROP_TYPE", "sblr.catalog.mutation.v3",
       "drop_type_descriptor", "ddl_result", true},
      {"catalog.type.show", "engine.op.catalog_introspect",
       "SBLR_CATALOG_INTROSPECT", "sblr.catalog.introspect.v3",
       "catalog_introspect_descriptor", "catalog_introspect_result", true},
      {"catalog.type.show_all", "engine.op.catalog_introspect",
       "SBLR_CATALOG_INTROSPECT", "sblr.catalog.introspect.v3",
       "catalog_introspect_descriptor", "catalog_introspect_result", true},
      {"query.structured_type.constructor", "engine.op.domain_operation",
       "SBLR_DOMAIN_OPERATION", "sblr.query.relational.v3",
       "domain_operation_descriptor", "typed_value", true},
      {"query.structured_type.cast", "engine.op.cast", "SBLR_CAST",
       "sblr.query.relational.v3", "cast_descriptor", "typed_value", true},
      {"query.structured_type.compare", "engine.op.compare", "SBLR_COMPARE",
       "sblr.query.relational.v3", "comparison_descriptor", "boolean_value", true},
      {"query.structured_type.serialize", "engine.op.domain_operation",
       "SBLR_DOMAIN_OPERATION", "sblr.query.relational.v3",
       "domain_operation_descriptor", "typed_value", true},
  };
  for (const auto& route : routes) {
    Require(sblr::LookupSblrOperation(route.retired_operation_id) == nullptr,
            "retired structured type synthetic SBLR identity remained admitted");
    const auto* entry = sblr::LookupSblrOperation(route.operation_id);
    Require(entry != nullptr, "structured type opcode registry entry missing");
    Require(entry->opcode == route.opcode, "structured type opcode mismatch");
    Require(entry->operand_contract == route.operand_contract,
            "structured type operand contract mismatch");
    Require(entry->result_contract == route.result_contract,
            "structured type result contract mismatch");
    Require(entry->support == sblr::SblrOpcodeSupport::implemented,
            "structured type opcode not implemented");
    Require(entry->requires_transaction_context == route.requires_txn,
            "structured type opcode transaction requirement drifted");
    const auto admission = server::AdmitServerSblrEnvelope(
        scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
            route.operation_id, route.opcode));
    if (!admission.admitted) {
      std::cerr << "route=" << route.operation_id << " family="
                << route.family << "\n";
      for (const auto& diagnostic : admission.diagnostics) {
        std::cerr << diagnostic.code << " " << diagnostic.message_key << "\n";
      }
    }
    Require(admission.admitted, "server rejected structured type SBLR route");
    Require(admission.operation_family == route.family,
            "server structured type route family mismatch");
  }
}

api::EngineCreateStructuredTypeRequest BaseCreate(const Fixture& fixture,
                                                  std::string type_uuid,
                                                  std::string family,
                                                  std::string name) {
  api::EngineCreateStructuredTypeRequest request;
  request.context = Context(fixture, {"TYPE_DDL", "USAGE"});
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = std::move(type_uuid);
  request.target_object.object_kind = "structured_type_descriptor";
  request.localized_names.push_back(Name(std::move(name)));
  request.option_envelopes.push_back("structured_family:" + std::move(family));
  return request;
}

void RequireCreateOk(const api::EngineCreateStructuredTypeResult& result,
                     std::string_view family) {
  Require(result.ok, "structured type create failed");
  Require(result.result_shape.result_kind == "rs.structured_type.descriptor.v1",
          "structured type create result shape drifted");
  Require(HasEvidence(result, "structured_family", family),
          "structured family evidence missing");
  Require(HasEvidence(result, "catalog_identity", "structured_type_uuidv7"),
          "structured type catalog identity evidence missing");
  Require(HasEvidence(result, "parser_executes_sql", "false"),
          "structured type API drifted into SQL parser authority");
}

void TestCreateFamilies(Fixture* fixture) {
  auto composite = BaseCreate(*fixture,
                              std::string(kCompositeUuid),
                              "composite",
                              "address_type");
  composite.option_envelopes.push_back("syntax_form:composite_native");
  composite.option_envelopes.push_back("field:id:uint64");
  composite.option_envelopes.push_back("field:street:text");
  RequireCreateOk(api::EngineCreateStructuredType(composite), "composite");

  auto shorthand = BaseCreate(*fixture,
                              std::string(kCompositeShorthandUuid),
                              "composite",
                              "point_type");
  shorthand.option_envelopes.push_back("syntax_form:composite_shorthand");
  shorthand.option_envelopes.push_back("field:x:int64");
  shorthand.option_envelopes.push_back("field:y:int64");
  RequireCreateOk(api::EngineCreateStructuredType(shorthand), "composite");

  auto enum_type = BaseCreate(*fixture,
                              std::string(kEnumUuid),
                              "enum",
                              "color_type");
  enum_type.option_envelopes.push_back("label:red");
  enum_type.option_envelopes.push_back("label:green");
  enum_type.option_envelopes.push_back("label:__SB_ENUM_TOMBSTONE__");
  RequireCreateOk(api::EngineCreateStructuredType(enum_type), "enum");

  auto pg_range = BaseCreate(*fixture,
                             std::string(kRangeUuid),
                             "range",
                             "business_time_range");
  pg_range.option_envelopes.push_back("syntax_form:pg_range");
  pg_range.option_envelopes.push_back("subtype:timestamp");
  pg_range.option_envelopes.push_back("range_option:canonical=closed_open");
  RequireCreateOk(api::EngineCreateStructuredType(pg_range), "range");

  auto native_range = BaseCreate(*fixture,
                                 std::string(kNativeRangeUuid),
                                 "range",
                                 "native_int_range");
  native_range.option_envelopes.push_back("syntax_form:native_range");
  native_range.option_envelopes.push_back("subtype:int64");
  RequireCreateOk(api::EngineCreateStructuredType(native_range), "range");

  auto multirange = BaseCreate(*fixture,
                               std::string(kMultirangeUuid),
                               "multirange",
                               "business_time_multirange");
  multirange.option_envelopes.push_back("auto_derived:true");
  multirange.option_envelopes.push_back("base_range_uuid:" + std::string(kRangeUuid));
  RequireCreateOk(api::EngineCreateStructuredType(multirange), "multirange");

  auto variant = BaseCreate(*fixture,
                            std::string(kVariantUuid),
                            "variant",
                            "measurement_variant");
  variant.option_envelopes.push_back("alternative:int_value:int64");
  variant.option_envelopes.push_back("alternative:text_value:text");
  RequireCreateOk(api::EngineCreateStructuredType(variant), "variant");

  auto set_type = BaseCreate(*fixture,
                             std::string(kSetUuid),
                             "set",
                             "feature_flag_set");
  set_type.option_envelopes.push_back("element_type:text");
  set_type.option_envelopes.push_back("member:alpha");
  set_type.option_envelopes.push_back("member:beta");
  RequireCreateOk(api::EngineCreateStructuredType(set_type), "set");

  api::EngineShowStructuredTypesRequest show_all;
  show_all.context = Context(*fixture, {"USAGE"});
  const auto list = api::EngineShowStructuredTypes(show_all);
  Require(list.ok, "SHOW TYPES failed");
  Require(list.result_shape.result_kind == "rs.structured_type.list.v1",
          "SHOW TYPES result shape drifted");
  Require(AnyFieldValue(list, "structured_family", "composite"),
          "composite type missing from SHOW TYPES");
  Require(AnyFieldValue(list, "structured_family", "enum"),
          "enum type missing from SHOW TYPES");
  Require(AnyFieldValue(list, "structured_family", "range"),
          "range type missing from SHOW TYPES");
  Require(AnyFieldValue(list, "structured_family", "multirange"),
          "multirange type missing from SHOW TYPES");
  Require(AnyFieldValue(list, "structured_family", "variant"),
          "variant type missing from SHOW TYPES");
  Require(AnyFieldValue(list, "structured_family", "set"),
          "set type missing from SHOW TYPES");

  api::EngineShowStructuredTypeRequest show_one;
  show_one.context = Context(*fixture, {"USAGE"});
  show_one.target_object.uuid.canonical = std::string(kCompositeUuid);
  show_one.target_object.object_kind = "structured_type_descriptor";
  const auto one = api::EngineShowStructuredType(show_one);
  Require(one.ok, "SHOW TYPE failed");
  Require(FieldValue(one, "type_uuid") == std::string(kCompositeUuid),
          "SHOW TYPE returned wrong UUID");
  Require(FieldValue(one, "field_count") == "2",
          "composite descriptor fields not projected");
}

void TestConstructorCastCompareSerialize(const Fixture& fixture) {
  api::EngineEvaluateStructuredTypeConstructorRequest construct_enum;
  construct_enum.context = Context(fixture, {"USAGE"});
  construct_enum.target_object.uuid.canonical = std::string(kEnumUuid);
  construct_enum.target_object.object_kind = "structured_type_descriptor";
  construct_enum.option_envelopes.push_back("label:green");
  construct_enum.option_envelopes.push_back("encoded_value:green");
  const auto enum_result =
      api::EngineEvaluateStructuredTypeConstructor(construct_enum);
  Require(enum_result.ok, "enum constructor failed");
  Require(HasEvidence(enum_result, "structured_constructor_registry", "active"),
          "constructor registry evidence missing");

  api::EngineEvaluateStructuredTypeCastRequest cast;
  cast.context = Context(fixture, {"USAGE"});
  cast.target_object.uuid.canonical = std::string(kRangeUuid);
  cast.target_object.object_kind = "structured_type_descriptor";
  cast.option_envelopes.push_back("source_descriptor:timestamp_pair");
  const auto cast_result = api::EngineEvaluateStructuredTypeCast(cast);
  Require(cast_result.ok, "structured type cast failed");
  Require(HasEvidence(cast_result, "structured_cast_registry", "active"),
          "cast registry evidence missing");

  api::EngineCompareStructuredTypeValuesRequest compare;
  compare.context = Context(fixture, {"USAGE"});
  compare.target_object.uuid.canonical = std::string(kVariantUuid);
  compare.target_object.object_kind = "structured_type_descriptor";
  const auto compare_result = api::EngineCompareStructuredTypeValues(compare);
  Require(compare_result.ok, "structured type compare failed");
  Require(HasEvidence(compare_result, "structured_comparison_registry", "active"),
          "comparison registry evidence missing");

  api::EngineSerializeStructuredTypeValueRequest serialize;
  serialize.context = Context(fixture, {"USAGE"});
  serialize.target_object.uuid.canonical = std::string(kCompositeUuid);
  serialize.target_object.object_kind = "structured_type_descriptor";
  const auto serialize_result = api::EngineSerializeStructuredTypeValue(serialize);
  Require(serialize_result.ok, "structured type serialization failed");
  Require(HasEvidence(serialize_result,
                      "structured_serialization_registry",
                      "active"),
          "serialization registry evidence missing");
  Require(FieldValue(serialize_result, "serialized_frame") == "SBTYPE1:composite",
          "structured serialization frame drifted");
}

void TestMutationsAndRefusals(Fixture* fixture) {
  auto direct_self = BaseCreate(*fixture,
                                "019f0600-0000-7000-8000-000000000210",
                                "composite",
                                "bad_self");
  direct_self.option_envelopes.push_back("field:self_ref:self");
  const auto self_result = api::EngineCreateStructuredType(direct_self);
  Require(!self_result.ok, "direct composite self recursion accepted");
  Require(HasDiagnostic(self_result,
                        "SBSQL.STRUCTURED_TYPE_DIRECT_RECURSION_REFUSED"),
          "direct self recursion diagnostic missing");

  auto duplicate_enum = BaseCreate(*fixture,
                                   "019f0600-0000-7000-8000-000000000211",
                                   "enum",
                                   "bad_enum");
  duplicate_enum.option_envelopes.push_back("label:dup");
  duplicate_enum.option_envelopes.push_back("label:dup");
  const auto duplicate_result = api::EngineCreateStructuredType(duplicate_enum);
  Require(!duplicate_result.ok, "duplicate enum label accepted");
  Require(HasDiagnostic(duplicate_result, "SBSQL.STRUCTURED_TYPE_DUPLICATE_MEMBER"),
          "duplicate member diagnostic missing");

  auto raw_sql = BaseCreate(*fixture,
                            "019f0600-0000-7000-8000-000000000212",
                            "enum",
                            "bad_sql_text");
  raw_sql.option_envelopes.push_back("label:a");
  raw_sql.option_envelopes.push_back("sql_text:CREATE TYPE should_not_execute");
  const auto raw_sql_result = api::EngineCreateStructuredType(raw_sql);
  Require(!raw_sql_result.ok, "raw SQL marker accepted by engine type API");
  Require(HasDiagnostic(raw_sql_result, "SB_ENGINE_API_INVALID_REQUEST"),
          "raw SQL diagnostic missing");

  api::EngineShowStructuredTypeRequest no_usage;
  no_usage.context = Context(*fixture, {});
  no_usage.target_object.uuid.canonical = std::string(kEnumUuid);
  no_usage.target_object.object_kind = "structured_type_descriptor";
  const auto no_usage_result = api::EngineShowStructuredType(no_usage);
  Require(!no_usage_result.ok, "SHOW TYPE without USAGE accepted");
  Require(HasDiagnostic(no_usage_result, "SECURITY.AUTHORIZATION.DENIED"),
          "USAGE authorization diagnostic missing");

  api::EngineAlterStructuredTypeRequest multi_alter;
  multi_alter.context = Context(*fixture, {"TYPE_DDL", "USAGE"});
  multi_alter.target_object.uuid.canonical = std::string(kEnumUuid);
  multi_alter.target_object.object_kind = "structured_type_descriptor";
  multi_alter.option_envelopes.push_back("mutation_count:2");
  multi_alter.option_envelopes.push_back("drop_label:red");
  const auto multi_result = api::EngineAlterStructuredType(multi_alter);
  Require(!multi_result.ok, "multi-clause ALTER TYPE accepted");
  Require(HasDiagnostic(multi_result,
                        "SBSQL.STRUCTURED_TYPE_MULTI_MUTATION_REFUSED"),
          "multi-clause ALTER TYPE diagnostic missing");

  api::EngineAlterStructuredTypeRequest drop_label;
  drop_label.context = Context(*fixture, {"TYPE_DDL", "USAGE"});
  drop_label.target_object.uuid.canonical = std::string(kEnumUuid);
  drop_label.target_object.object_kind = "structured_type_descriptor";
  drop_label.option_envelopes.push_back("mutation_count:1");
  drop_label.option_envelopes.push_back("drop_label:green");
  const auto drop_label_result = api::EngineAlterStructuredType(drop_label);
  Require(drop_label_result.ok, "enum DROP VALUE tombstone failed");
  Require(HasEvidence(drop_label_result, "enum_tombstone", "green"),
          "enum tombstone evidence missing");

  api::EngineEvaluateStructuredTypeConstructorRequest retired_enum;
  retired_enum.context = Context(*fixture, {"USAGE"});
  retired_enum.target_object.uuid.canonical = std::string(kEnumUuid);
  retired_enum.target_object.object_kind = "structured_type_descriptor";
  retired_enum.option_envelopes.push_back("label:green");
  const auto retired_result =
      api::EngineEvaluateStructuredTypeConstructor(retired_enum);
  Require(!retired_result.ok, "retired enum label accepted for new value");
  Require(HasDiagnostic(retired_result, "SBSQL.STRUCTURED_ENUM_LABEL_RETIRED"),
          "retired enum label diagnostic missing");

  api::EngineAlterStructuredTypeRequest alter_range;
  alter_range.context = Context(*fixture, {"TYPE_DDL", "USAGE"});
  alter_range.target_object.uuid.canonical = std::string(kRangeUuid);
  alter_range.target_object.object_kind = "structured_type_descriptor";
  alter_range.option_envelopes.push_back("mutation_count:1");
  alter_range.option_envelopes.push_back("set_range_option:canonical=lower_inc");
  const auto range_result = api::EngineAlterStructuredType(alter_range);
  Require(range_result.ok, "range option mutation failed");
  Require(HasEvidence(range_result, "range_recanonicalization", "required"),
          "range recanonicalization evidence missing");

  api::EngineDropStructuredTypeRequest drop_set;
  drop_set.context = Context(*fixture, {"TYPE_DDL", "USAGE"});
  drop_set.target_object.uuid.canonical = std::string(kSetUuid);
  drop_set.target_object.object_kind = "structured_type_descriptor";
  const auto drop_result = api::EngineDropStructuredType(drop_set);
  Require(drop_result.ok, "DROP TYPE failed");
  Require(HasEvidence(drop_result, "audit_event", "catalog.structured_type.dropped"),
          "DROP TYPE audit evidence missing");

  api::EngineShowStructuredTypeRequest show_dropped;
  show_dropped.context = Context(*fixture, {"USAGE"});
  show_dropped.target_object.uuid.canonical = std::string(kSetUuid);
  show_dropped.target_object.object_kind = "structured_type_descriptor";
  const auto show_dropped_result = api::EngineShowStructuredType(show_dropped);
  Require(!show_dropped_result.ok, "dropped structured type stayed visible");
  Require(HasDiagnostic(show_dropped_result, "SBSQL.STRUCTURED_TYPE_NOT_FOUND"),
          "dropped type not-found diagnostic missing");
}

void TestCommittedReopenVisibility(Fixture* fixture) {
  CommitFixture(fixture);
  api::EngineShowStructuredTypeRequest show_enum;
  show_enum.context = Context(*fixture, {"USAGE"});
  show_enum.target_object.uuid.canonical = std::string(kEnumUuid);
  show_enum.target_object.object_kind = "structured_type_descriptor";
  const auto enum_result = api::EngineShowStructuredType(show_enum);
  Require(enum_result.ok, "committed enum descriptor not visible after reopen");
  Require(FieldValue(enum_result, "retired_labels") == "green",
          "enum tombstone not durable after commit/reopen");

  api::EngineShowStructuredTypeRequest show_range;
  show_range.context = Context(*fixture, {"USAGE"});
  show_range.target_object.uuid.canonical = std::string(kRangeUuid);
  show_range.target_object.object_kind = "structured_type_descriptor";
  const auto range_result = api::EngineShowStructuredType(show_range);
  Require(range_result.ok, "committed range descriptor not visible after reopen");
  Require(FieldValue(range_result, "structured_family") == "range",
          "range descriptor family drifted after reopen");
}

}  // namespace

int main() {
  ValidateAdmissionAndRegistry();
  Fixture fixture = CreateFixture();
  TestCreateFamilies(&fixture);
  TestConstructorCastCompareSerialize(fixture);
  TestMutationsAndRefusals(&fixture);
  TestCommittedReopenVisibility(&fixture);
  Cleanup(fixture.path);
  return EXIT_SUCCESS;
}
