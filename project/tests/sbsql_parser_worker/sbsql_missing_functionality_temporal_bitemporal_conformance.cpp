// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/temporal_bitemporal_api.hpp"
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
    "019f0500-0000-7000-8000-000000000001";
constexpr std::string_view kTableUuid =
    "019f0500-0000-7000-8000-000000000003";
constexpr std::string_view kSystemPeriodUuid =
    "019f0500-0000-7000-8000-000000000004";
constexpr std::string_view kApplicationPeriodUuid =
    "019f0500-0000-7000-8000-000000000005";

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
    if (diagnostic.code == code) {
      return true;
    }
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
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
  }
  return {};
}

bool AnyFieldValue(const api::EngineApiResult& result,
                   std::string_view field,
                   std::string_view expected) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    if (FieldValue(result, field, i) == expected) {
      return true;
    }
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "canonical";
  localized.path = "users.public.temporal";
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

void Grant(api::EngineRequestContext* context,
           std::string right,
           std::string target_uuid = std::string(kTableUuid)) {
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
  context.session_uuid.canonical = "019f0500-0000-7000-8000-000000000010";
  context.transaction_uuid.canonical = fixture.transaction_uuid;
  context.local_transaction_id = fixture.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      fixture.local_transaction_id;
  context.security_context_present = true;
  context.authorization_context.present = true;
  context.authorization_context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.authorization_context.authority_uuid.canonical =
      "019f0500-0000-7000-8000-000000000020";
  context.authorization_context.evidence_tags.push_back("temporal_conformance");
  for (auto& right : rights) {
    Grant(&context, std::move(right));
  }
  return context;
}

Fixture CreateFixture() {
  Fixture fixture;
  fixture.path = std::filesystem::temp_directory_path() /
                 "scratchbird_sbsql_miss005_temporal.sbdb";
  Cleanup(fixture.path);

  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1790500000000);
  Require(database_uuid.ok(), "database UUID generation failed");
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1790500000001);
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790500000002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database creation failed");

  fixture.inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790500000003);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = txn::BeginLocalTransaction(std::move(fixture.inventory),
                                          transaction_uuid.value,
                                          1790500000004);
  Require(begun.ok(), "local transaction begin failed");
  fixture.inventory = std::move(begun.inventory);
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  fixture.local_transaction_id = begun.entry.identity.local_id.value;
  fixture.typed_local_transaction_id = begun.entry.identity.local_id;
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(fixture.path.string(),
                                                     fixture.inventory);
  Require(persisted.ok(), "active transaction inventory persist failed");
  return fixture;
}

void CommitFixture(Fixture* fixture) {
  auto committed = txn::CommitLocalTransaction(std::move(fixture->inventory),
                                               fixture->typed_local_transaction_id,
                                               1790500000100);
  Require(committed.ok(), "local transaction commit failed");
  const auto reopen_transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1790500000101);
  Require(reopen_transaction_uuid.ok(), "reopen transaction UUID generation failed");
  auto reopened = txn::BeginLocalTransaction(std::move(committed.inventory),
                                             reopen_transaction_uuid.value,
                                             1790500000102);
  Require(reopened.ok(), "reopen transaction begin failed");
  fixture->inventory = std::move(reopened.inventory);
  fixture->transaction_uuid =
      uuid::UuidToString(reopen_transaction_uuid.value.value);
  fixture->local_transaction_id = reopened.entry.identity.local_id.value;
  fixture->typed_local_transaction_id = reopened.entry.identity.local_id;
  const auto persisted =
      db::PersistLocalTransactionInventoryToDatabase(fixture->path.string(),
                                                     fixture->inventory);
  Require(persisted.ok(), "reopen transaction inventory persist failed");
}

void ValidateAdmissionAndRegistry() {
  struct ExpectedRoute {
    std::string retired_operation_id;
    std::string operation_id;
    std::string opcode;
    std::string family;
  };
  const ExpectedRoute routes[] = {
      {"versioned.bitemporal.show_periods",
       "engine.op.catalog_introspect",
       "SBLR_CATALOG_INTROSPECT",
       "sblr.catalog.introspect.v3"},
      {"versioned.bitemporal.show_history",
       "engine.op.bitemporal_for_versions_between",
       "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN",
       "sblr.versioned.history.read.v3"},
      {"dml.for_portion_of_period",
       "engine.op.update",
       "SBLR_UPDATE",
       "sblr.dml.update.v3"},
      {"dml.for_portion_of_period",
       "engine.op.delete",
       "SBLR_DELETE",
       "sblr.dml.delete.v3"},
  };
  for (const auto& route : routes) {
    Require(sblr::LookupSblrOperation(route.retired_operation_id) == nullptr,
            "retired temporal synthetic SBLR identity remained admitted");
    const auto* entry = sblr::LookupSblrOperation(route.operation_id);
    Require(entry != nullptr, "temporal opcode registry entry missing");
    Require(entry->opcode == route.opcode, "temporal opcode name mismatch");
    Require(entry->support == sblr::SblrOpcodeSupport::implemented,
            "temporal opcode not implemented");
    const auto admission = server::AdmitServerSblrEnvelope(
        scratchbird::test::sbsql::BuildCanonicalSblrAdmissionRequest(
            route.operation_id, route.opcode));
    if (!admission.admitted) {
      std::cerr << "route=" << route.operation_id << " expected_family="
                << route.family << "\n";
      for (const auto& diagnostic : admission.diagnostics) {
        std::cerr << diagnostic.code << " " << diagnostic.message_key << "\n";
      }
    }
    Require(admission.admitted, "server rejected temporal SBLR route");
    Require(admission.operation_family == route.family,
            "server temporal route family mismatch");
  }
}

api::EngineCreateTemporalPeriodRequest BaseCreateRequest(const Fixture& fixture,
                                                         std::string period_uuid,
                                                         std::string name) {
  api::EngineCreateTemporalPeriodRequest request;
  request.context = Context(fixture,
                            {"TEMPORAL_HISTORY_ADMIN",
                             "TEMPORAL_HISTORY_READ",
                             "TEMPORAL_BACKDATE"});
  request.target_object.uuid.canonical = std::move(period_uuid);
  request.target_object.object_kind = "temporal_period";
  request.related_objects.push_back({{std::string(kTableUuid)}, "table"});
  request.localized_names.push_back(Name(std::move(name)));
  request.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  return request;
}

void TestCreateAndShowPeriods(Fixture* fixture) {
  auto system = BaseCreateRequest(*fixture,
                                  std::string(kSystemPeriodUuid),
                                  "system_period");
  system.option_envelopes.push_back("axis:system_time");
  system.option_envelopes.push_back("system_versioning:true");
  system.option_envelopes.push_back("start_generated_always:true");
  system.option_envelopes.push_back("end_generated_always:true");
  system.option_envelopes.push_back("history_table_visible:false");
  system.option_envelopes.push_back("history_retention:P30D");
  const auto system_result = api::EngineCreateTemporalPeriod(system);
  Require(system_result.ok, "system-time period create failed");
  Require(HasEvidence(system_result, "audit_event",
                      "catalog.bitemporal_period.added"),
          "system-time create audit evidence missing");
  Require(HasEvidence(system_result, "parser_executes_sql", "false"),
          "temporal API drifted into parser SQL authority");

  auto application = BaseCreateRequest(*fixture,
                                       std::string(kApplicationPeriodUuid),
                                       "valid_range");
  application.option_envelopes.push_back("axis:valid_time");
  application.option_envelopes.push_back("start_bound_type:timestamp");
  application.option_envelopes.push_back("end_bound_type:timestamp");
  application.option_envelopes.push_back("history_table_visible:true");
  const auto application_result = api::EngineCreateTemporalPeriod(application);
  Require(application_result.ok, "application-time period create failed");

  api::EngineShowBitemporalPeriodsRequest show;
  show.context = Context(*fixture, {"TEMPORAL_HISTORY_READ"});
  show.target_object.uuid.canonical = std::string(kTableUuid);
  show.target_object.object_kind = "table";
  show.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  const auto show_result = api::EngineShowBitemporalPeriods(show);
  Require(show_result.ok, "show bitemporal periods failed");
  Require(show_result.result_shape.result_kind == "rs.bitemporal.periods.v1",
          "period result shape drifted");
  Require(AnyFieldValue(show_result, "axis", "system_time"),
          "system-time axis not projected");
  Require(AnyFieldValue(show_result, "axis", "application_time"),
          "VALID_TIME was not canonicalized to application_time");
  Require(AnyFieldValue(show_result,
                        "period_descriptor_storage",
                        "table_descriptor_field"),
          "period descriptor storage proof missing");
}

void TestTemporalRefusals(const Fixture& fixture) {
  auto writable_system = BaseCreateRequest(fixture,
                                           "019f0500-0000-7000-8000-000000000040",
                                           "bad_system_period");
  writable_system.option_envelopes.push_back("axis:system_time");
  writable_system.option_envelopes.push_back("start_generated_always:true");
  writable_system.option_envelopes.push_back("end_generated_always:true");
  writable_system.option_envelopes.push_back("system_time_user_writable:true");
  const auto writable_result = api::EngineCreateTemporalPeriod(writable_system);
  Require(!writable_result.ok, "writable system-time bounds accepted");
  Require(HasDiagnostic(writable_result,
                        "SBSQL.TEMPORAL_SYSTEM_TIME_GENERATED_ALWAYS_REQUIRED"),
          "system-time generated diagnostic missing");

  auto mixed_bounds = BaseCreateRequest(fixture,
                                        "019f0500-0000-7000-8000-000000000041",
                                        "bad_application_period");
  mixed_bounds.option_envelopes.push_back("axis:application_time");
  mixed_bounds.option_envelopes.push_back("start_bound_type:date");
  mixed_bounds.option_envelopes.push_back("end_bound_type:timestamp");
  const auto mixed_result = api::EngineCreateTemporalPeriod(mixed_bounds);
  Require(!mixed_result.ok, "mixed application-time bounds accepted");
  Require(HasDiagnostic(mixed_result, "SBSQL.TEMPORAL_BOUND_TYPE_MISMATCH"),
          "mixed bound diagnostic missing");

  api::EngineReadBitemporalHistoryRequest repeated;
  repeated.context = Context(fixture, {"TEMPORAL_HISTORY_READ"});
  repeated.target_object.uuid.canonical = std::string(kTableUuid);
  repeated.target_object.object_kind = "table";
  repeated.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  repeated.option_envelopes.push_back("axis:system_time");
  repeated.option_envelopes.push_back("axis:system");
  const auto repeated_result = api::EngineReadBitemporalHistory(repeated);
  Require(!repeated_result.ok, "repeated temporal axis accepted");
  Require(HasDiagnostic(repeated_result, "SBSQL.TEMPORAL_AXIS_REPEATED"),
          "repeated axis diagnostic missing");

  api::EngineReadBitemporalHistoryRequest reversed;
  reversed.context = Context(fixture, {"TEMPORAL_HISTORY_READ"});
  reversed.target_object.uuid.canonical = std::string(kTableUuid);
  reversed.target_object.object_kind = "table";
  reversed.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  reversed.option_envelopes.push_back("axis:system_time");
  reversed.option_envelopes.push_back("system_time_from:2026-06-02T00:00:00Z");
  reversed.option_envelopes.push_back("system_time_to:2026-06-01T00:00:00Z");
  const auto reversed_result = api::EngineReadBitemporalHistory(reversed);
  Require(!reversed_result.ok, "reversed temporal range accepted");
  Require(HasDiagnostic(reversed_result, "SBSQL.TEMPORAL_RANGE_REVERSED"),
          "reversed range diagnostic missing");

  api::EngineDropTemporalPeriodRequest drop;
  drop.context = Context(fixture, {"TEMPORAL_HISTORY_ADMIN"});
  drop.target_object.uuid.canonical = std::string(kSystemPeriodUuid);
  drop.target_object.object_kind = "temporal_period";
  drop.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  const auto drop_result = api::EngineDropTemporalPeriod(drop);
  Require(!drop_result.ok, "DROP PERIOD without disposition accepted");
  Require(HasDiagnostic(drop_result,
                        "SBSQL.TEMPORAL_HISTORY_DISPOSITION_REQUIRED"),
          "DROP PERIOD disposition diagnostic missing");

  api::EngineShowBitemporalPeriodsRequest no_right;
  no_right.context = Context(fixture, {});
  no_right.target_object.uuid.canonical = std::string(kTableUuid);
  no_right.target_object.object_kind = "table";
  const auto no_right_result = api::EngineShowBitemporalPeriods(no_right);
  Require(!no_right_result.ok, "temporal read without right accepted");
  Require(HasDiagnostic(no_right_result, "SECURITY.AUTHORIZATION.DENIED"),
          "temporal authorization diagnostic missing");
}

void TestReadAndDml(Fixture* fixture) {
  api::EngineReadBitemporalHistoryRequest read;
  read.context = Context(*fixture, {"TEMPORAL_HISTORY_READ"});
  read.target_object.uuid.canonical = std::string(kTableUuid);
  read.target_object.object_kind = "table";
  read.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  read.option_envelopes.push_back("axis:system_time");
  read.option_envelopes.push_back("axis:application_time");
  read.option_envelopes.push_back("system_time_from:2026-06-01T00:00:00Z");
  read.option_envelopes.push_back("system_time_to:2026-06-02T00:00:00Z");
  read.option_envelopes.push_back("application_time_from:2026-01-01");
  read.option_envelopes.push_back("application_time_to:2026-12-31");
  const auto read_result = api::EngineReadBitemporalHistory(read);
  Require(read_result.ok, "two-axis bitemporal read failed");
  Require(HasEvidence(read_result,
                      "sblr_opcode",
                      "SBLR_BITEMPORAL_FOR_VERSIONS_BETWEEN"),
          "two-axis fused opcode evidence missing");
  Require(FieldValue(read_result, "temporal_filter_order") == "temporal_before_rls",
          "temporal/RLS ordering proof missing");
  Require(HasEvidence(read_result, "mask_evaluation_timestamp",
                      "live_time_default"),
          "mask timestamp proof missing");

  api::EngineApplyForPortionOfPeriodRequest denied_backdate;
  denied_backdate.context = Context(*fixture, {"TEMPORAL_HISTORY_READ"});
  denied_backdate.target_object.uuid.canonical =
      "019f0500-0000-7000-8000-000000000050";
  denied_backdate.target_object.object_kind = "temporal_dml_event";
  denied_backdate.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  denied_backdate.option_envelopes.push_back("period_uuid:" + std::string(kApplicationPeriodUuid));
  denied_backdate.option_envelopes.push_back("application_time_from:2025-01-01");
  denied_backdate.option_envelopes.push_back("application_time_to:2025-02-01");
  denied_backdate.option_envelopes.push_back("dml_action:update");
  denied_backdate.option_envelopes.push_back("backdate:true");
  const auto denied_result =
      api::EngineApplyForPortionOfPeriod(denied_backdate);
  Require(!denied_result.ok, "backdated FOR PORTION accepted without right");
  Require(HasDiagnostic(denied_result, "TEMPORAL.BACKDATE_RIGHT_REQUIRED"),
          "backdate right diagnostic missing");

  api::EngineApplyForPortionOfPeriodRequest apply = denied_backdate;
  apply.context = Context(*fixture, {"TEMPORAL_HISTORY_READ",
                                    "TEMPORAL_BACKDATE",
                                    "TEMPORAL_HISTORY_ADMIN"});
  const auto apply_result = api::EngineApplyForPortionOfPeriod(apply);
  Require(apply_result.ok, "FOR PORTION OF DML failed");
  Require(HasEvidence(apply_result,
                      "sblr_opcode",
                      "SBLR_UPDATE"),
          "FOR PORTION OF opcode evidence missing");
  Require(HasEvidence(apply_result,
                      "audit_event",
                      "dml.for_portion_of_period.applied"),
          "FOR PORTION OF privileged audit evidence missing");

  api::EngineApplyForPortionOfPeriodRequest delete_portion = apply;
  delete_portion.target_object.uuid.canonical =
      "019f0500-0000-7000-8000-000000000051";
  delete_portion.option_envelopes.erase(
      std::remove(delete_portion.option_envelopes.begin(),
                  delete_portion.option_envelopes.end(),
                  "dml_action:update"),
      delete_portion.option_envelopes.end());
  delete_portion.option_envelopes.push_back("dml_action:delete");
  const auto delete_result = api::EngineApplyForPortionOfPeriod(delete_portion);
  Require(delete_result.ok, "DELETE FOR PORTION OF DML failed");
  Require(HasEvidence(delete_result, "sblr_opcode", "SBLR_DELETE"),
          "DELETE FOR PORTION OF opcode evidence missing");

  api::EngineShowBitemporalHistoryRequest history;
  history.context = Context(*fixture, {"TEMPORAL_HISTORY_READ"});
  history.target_object.uuid.canonical = std::string(kTableUuid);
  history.target_object.object_kind = "table";
  history.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  const auto history_result = api::EngineShowBitemporalHistory(history);
  Require(history_result.ok, "SHOW BITEMPORAL HISTORY failed");
  Require(history_result.result_shape.result_kind == "rs.bitemporal.history.v1",
          "history result shape drifted");
  Require(AnyFieldValue(history_result,
                        "period_uuid",
                        std::string(kApplicationPeriodUuid)),
          "history did not include application period evidence");
}

void TestCommittedReopenVisibility(Fixture* fixture) {
  CommitFixture(fixture);
  api::EngineShowBitemporalPeriodsRequest show;
  show.context = Context(*fixture, {"TEMPORAL_HISTORY_READ"});
  show.target_object.uuid.canonical = std::string(kTableUuid);
  show.target_object.object_kind = "table";
  show.option_envelopes.push_back("table_uuid:" + std::string(kTableUuid));
  const auto result = api::EngineShowBitemporalPeriods(show);
  Require(result.ok, "committed reopen period show failed");
  Require(AnyFieldValue(result, "period_uuid", std::string(kSystemPeriodUuid)),
          "committed system period not visible after reopen");
  Require(AnyFieldValue(result, "period_uuid", std::string(kApplicationPeriodUuid)),
          "committed application period not visible after reopen");
}

}  // namespace

int main() {
  ValidateAdmissionAndRegistry();
  Fixture fixture = CreateFixture();
  TestCreateAndShowPeriods(&fixture);
  TestTemporalRefusals(fixture);
  TestReadAndDml(&fixture);
  TestCommittedReopenVisibility(&fixture);
  Cleanup(fixture.path);
  return EXIT_SUCCESS;
}
