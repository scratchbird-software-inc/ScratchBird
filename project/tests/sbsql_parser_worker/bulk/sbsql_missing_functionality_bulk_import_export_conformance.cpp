// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "ast/ast.hpp"
#include "artifacts/artifact_api.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "ddl/create_api.hpp"
#include "dml/import_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/import_reject_model.hpp"
#include "dml/import_resume_checkpoint.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "rendering/rendering.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../../database_lifecycle/credentialed_database_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace parser = scratchbird::parser::sbsql;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

namespace {

#ifndef SB_MISS009_SEED_PACK_ROOT
#define SB_MISS009_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2900-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2900-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2900-0000-7000-8000-000000000102";
constexpr const char* kUniqueIdIndexUuid = "019f2900-0000-7000-8000-000000000103";
constexpr const char* kArtifactUuid = "019f2900-0000-7000-8000-000000000901";
constexpr const char* kSourceUuid = "019f2900-0000-7000-8000-000000000903";
// The authenticated SBPS/public-ABI row, full IPEV, and no-query-handle leg is
// exhaustively owned by this adjacent non-QOW target. This executable proves
// its own parser surface and exact typed engine-dispatch leg.
constexpr std::string_view kPlanImportPublicAbiProofTarget =
    "sbsql_sblr_alignment_plan_import_rows_sbps_coordination";

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_missing_functionality_bulk_import_export_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_missing_functionality_bulk_import_export_conformance");
  Require(configured.ok(), "MISS-009 memory fixture configuration failed");
  Require(configured.fixture_mode, "MISS-009 memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_miss009_bulk.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
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

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineTypedValue TextValue(std::string value, bool is_null = false) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = is_null ? "type=text;nullable=true"
                                                : "type=text;nullable=false";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

api::EngineTypedValue BoolValue(bool value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "boolean";
  typed.descriptor.encoded_descriptor = "type=boolean;nullable=false";
  typed.encoded_value = value ? "true" : "false";
  return typed;
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2900-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2900-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kUniqueIdIndexUuid;
  index.names.push_back(Name("miss009_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRowValue ArtifactRow() {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = "019f2900-0000-7000-8000-000000000902";
  row.fields.push_back({"artifact_format", TextValue("sb.catalog.artifact.v1")});
  row.fields.push_back({"object_uuid", TextValue(kArtifactUuid)});
  row.fields.push_back({"object_kind", TextValue("bulk_runtime_probe")});
  row.fields.push_back({"default_name", TextValue("miss009_bulk_runtime_probe")});
  row.fields.push_back({"payload", TextValue("state=active;source=SBSQL-MISS-009")});
  row.fields.push_back({"remap_uuid", TextValue({}, true)});
  row.fields.push_back({"content_hash", TextValue({}, true)});
  row.fields.push_back({"value_redacted", BoolValue(false)});
  return row;
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

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

void Grant(api::EngineRequestContext* context,
           std::string right,
           std::string target_uuid = kTableUuid) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      "019f2900-0000-7000-8000-0000000006" +
      std::to_string(context->authorization_context.grants.size());
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = std::move(target_uuid);
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

void AddAuthorization(api::EngineRequestContext* context) {
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid = context->database_uuid;
  context->authorization_context.principal_uuid = context->principal_uuid;
  context->authorization_context.security_epoch = context->security_epoch;
  context->authorization_context.policy_epoch = 1;
  context->authorization_context.catalog_generation_id =
      context->catalog_generation_id;
  context->authorization_context.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  Grant(context, "INSERT");
  Grant(context, "SELECT");
  Grant(context, "CATALOG_MUTATE", "*");
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string_view session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "miss009-bulk-import-export";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2900-0000-7000-8000-000000000002";
  context.session_uuid.canonical =
      std::string("019f2900-0000-7000-8000-000000000") +
      std::string(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("SBSQL-MISS-009");
  return context;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string_view session_suffix,
                                           bool authorized = true) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_suffix);
  auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "MISS-009 transaction begin failed");
  auto context = BaseContext(database_path, session_suffix);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  if (authorized) AddAuthorization(&context);
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "MISS-009 commit failed");
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, "101", false);

  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("miss009_schema"));
  auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "MISS-009 schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid.canonical = kTableUuid;
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("miss009_table"));
  table_request.table_columns.push_back(Column(0, "id"));
  table_request.table_columns.push_back(Column(1, "note"));
  table_request.table_indexes.push_back(UniqueIdIndex());
  auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "MISS-009 table create failed");
  Commit(context);
}

std::string PlanFixtureUuid(std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::object, 1947000000000ull + salt);
  Require(generated.ok(), "MISS-009 plan fixture UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineRequestContext AttachPlanStatementAuthority(
    api::EngineRequestContext context) {
  const auto salt = context.local_transaction_id * 32;
  context.statement_uuid.canonical = PlanFixtureUuid(salt + 1);
  context.statement_snapshot_uuid.canonical.clear();
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  Require(snapshot.ok, "MISS-009 plan statement snapshot publication failed");
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.statement_snapshot_generation =
      snapshot.snapshot_vector.publication_inventory_next_local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_receipt_uuid.canonical = PlanFixtureUuid(salt + 2);
  context.statement_metadata_snapshot_uuid.canonical = PlanFixtureUuid(salt + 3);
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_active_excluded_local_transaction_ids =
      snapshot.snapshot_vector.active_excluded_local_transaction_ids;
  context.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      snapshot.snapshot_vector.in_doubt_excluded_local_transaction_ids;
  context.transaction_policy_snapshot_uuid.canonical = PlanFixtureUuid(salt + 4);
  context.transaction_policy_snapshot_generation = 1;
  context.resource_admission_uuid.canonical = PlanFixtureUuid(salt + 5);
  context.authorization_context.security_context_generation = 1;
  context.authorization_context.authority_uuid.canonical = PlanFixtureUuid(salt + 6);
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = 1;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  return context;
}

api::SblrExecutorAvailabilityRowIdentity PlanAvailabilityIdentity() {
  return {api::kSblrDmlPlanImportRowsExecutorId,
          api::kSblrDmlPlanImportRowsOpcodeCode,
          api::kSblrDmlPlanImportRowsOpcodeVersion,
          api::kSblrDmlPlanImportRowsOperandDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorId,
          api::kSblrDmlPlanImportRowsResultDescriptorVersion};
}

bool HasExactEnvelopeRefusal(const sblr::SblrDispatchResult& result,
                             std::string_view root_code) {
  return result.diagnostics.size() == 1 &&
         result.diagnostics.front().code == root_code &&
         result.api_result.diagnostics.size() == 1 &&
         result.api_result.diagnostics.front().code ==
             "SB_SBLR_DISPATCH_ENVELOPE_REJECTED";
}

bool HasExactIdentityEnvelopeRefusal(const sblr::SblrDispatchResult& result) {
  return !result.diagnostics.empty() &&
         result.diagnostics.front().code == "SBLR.OPCODE_INVALID" &&
         std::all_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto& diagnostic) {
                       return diagnostic.code == "SBLR.OPCODE_INVALID" ||
                              diagnostic.code == "SBLR.OPERAND_INVALID";
                     }) &&
         result.api_result.diagnostics.size() == 1 &&
         result.api_result.diagnostics.front().code ==
             "SB_SBLR_DISPATCH_ENVELOPE_REJECTED";
}

sblr::SblrOperationEnvelope ExactPlanEnvelope(
    const sblr::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  std::vector<std::uint8_t> descriptor_ref_bytes;
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(
              descriptor_ref, &descriptor_ref_bytes, &diagnostic) &&
              descriptor_ref_bytes.size() ==
                  sblr::kPlanImportRowsDescriptorRefBytesV1,
          "MISS-009 exact descriptor reference encoding failed");

  auto envelope = sblr::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "miss009.plan_import_rows.typed_dispatch");
  envelope.opcode_code = 793;
  envelope.operation_version_major = 1;
  envelope.operation_version_minor = 0;
  envelope.result_shape = "import_plan_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid = PlanFixtureUuid(1001);
  envelope.registry_snapshot_uuid = PlanFixtureUuid(1002);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body = std::move(descriptor_ref_bytes);
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

api::EnginePlanImportRowsResult VerifyImportPlanning(
    const api::EngineRequestContext& transaction_context) {
  auto context = AttachPlanStatementAuthority(transaction_context);
  const auto installed = api::LoadSblrExecutorAvailabilitySnapshot(
      context, PlanAvailabilityIdentity());
  Require(installed.ok && installed.snapshot.installed &&
              installed.snapshot.generation != 0,
          "MISS-009 plan executor availability bootstrap failed");
  const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      context, PlanAvailabilityIdentity());
  Require(current.ok && current.snapshot.installed &&
              current.snapshot.generation == installed.snapshot.generation,
          "MISS-009 current plan executor availability missing");

  auto binder_context = context;
  binder_context.trace_tags = {"private_dml_plan_import_rows_binder"};
  api::EngineCreateImportRowsPlanDescriptorRequestV1 bind;
  bind.context = binder_context;
  bind.structural_occurrence_id = 1;
  bind.target_table_uuid.canonical = kTableUuid;
  bind.source_kind = sblr::PlanImportRowsSourceKindV1::csv_stream;
  bind.source_fingerprint_present = false;
  bind.mappings.clear();
  bind.format_family = sblr::PlanImportRowsFormatFamilyV1::csv;
  bind.reject_mode = sblr::PlanImportRowsRejectModeV1::fail_fast;
  bind.reject_payload_policy =
      sblr::PlanImportRowsRejectPayloadPolicyV1::diagnostic_only;
  bind.resume_policy = sblr::PlanImportRowsResumePolicyV1::fail_closed;
  bind.strict_bulk_load_requested = false;
  bind.reference_relaxed_semantics_requested = false;
  bind.reference_relaxed_semantics_authorized = false;
  bind.reject_limit_ppm = 0;
  bind.reject_limit_rows = 0;
  const auto bound =
      api::CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(bind);
  if (!bound.ok) {
    std::cerr << bound.diagnostic.code << ':' << bound.diagnostic.detail << '\n';
  }
  Require(bound.ok, "MISS-009 exact engine import binder failed");

  auto consumer_context = context;
  consumer_context.trace_tags = {"private_dml_plan_import_rows_consumer"};
  sblr::SblrDispatchRequest request;
  request.context = consumer_context;
  request.envelope = ExactPlanEnvelope(bound.descriptor_ref);
  request.standalone_package_root = true;
  Require(request.envelope.opcode_code == 793 &&
              request.envelope.operation_version_major == 1 &&
              request.envelope.operation_version_minor == 0 &&
              request.envelope.result_shape == "import_plan_result" &&
              request.envelope.operands.size() == 1 &&
              request.envelope.operands.front().ordinal == 1 &&
              request.envelope.operands.front().type ==
                  "import_rows_plan_descriptor" &&
              request.envelope.operands.front().name == "request" &&
              request.envelope.operands.front().value.empty() &&
              request.envelope.operands.front().value_body.size() == 24,
          "MISS-009 exact 793/v1 descriptor-ref envelope drifted");
  const std::string opaque_operand(
      request.envelope.operands.front().value_body.begin(),
      request.envelope.operands.front().value_body.end());
  Require(opaque_operand.find(kTableUuid) == std::string::npos &&
              opaque_operand.find("miss009_table") == std::string::npos &&
              opaque_operand.find("LOAD") == std::string::npos,
          "MISS-009 exact descriptor-ref leaked UUID/name/SQL text");
  const auto dispatched = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : dispatched.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : dispatched.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(dispatched.envelope_validated && dispatched.accepted &&
              dispatched.dispatched_to_api && dispatched.api_result.ok &&
              dispatched.plan_import_rows_result.has_value(),
          "MISS-009 exact 793/v1 typed dispatch failed");
  const auto plan = *dispatched.plan_import_rows_result;
  Require(plan.surface_accepted && plan.planning_only &&
              plan.execution_requires_execute_import_rows &&
              !plan.row_execution_completed && !plan.row_persistence_claimed,
          "MISS-009 import planning-only result contract drifted");
  Require(plan.normalized_insert_mode_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsInsertModeV1::copy_import) &&
              plan.normalized_source_kind_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsSourceKindV1::csv_stream) &&
              plan.normalized_format_family_code ==
                  static_cast<std::uint16_t>(
                      sblr::PlanImportRowsFormatFamilyV1::csv) &&
              plan.mapped_column_count == 0 &&
              !plan.validated_request_descriptor_uuid.canonical.empty() &&
              plan.validated_request_descriptor_generation ==
                  bound.descriptor_ref.descriptor_generation &&
              std::any_of(
                  plan.validated_request_projection_sha256.begin(),
                  plan.validated_request_projection_sha256.end(),
                  [](std::uint8_t byte) { return byte != 0; }) &&
              plan.accepted_executor_evidence.exact_bytes.size() ==
                  sblr::kPlanImportRowsExecutorEvidenceBytesV1 &&
              plan.accepted_executor_evidence.request_descriptor_uuid ==
                  bound.descriptor_ref.descriptor_uuid &&
              plan.accepted_executor_evidence.request_descriptor_generation ==
                  bound.descriptor_ref.descriptor_generation &&
              plan.accepted_executor_evidence.executor_availability_generation ==
                  current.snapshot.generation &&
              plan.accepted_executor_evidence.completed_validation_bits ==
                  sblr::kPlanImportRowsAcceptedValidationBitsV1,
          "MISS-009 exact descriptor/result/IPEV fields drifted");
  Require(plan.evidence.size() == 1 &&
              plan.evidence.front().evidence_kind ==
                  "accepted_executor_evidence" &&
              plan.evidence.front().evidence_id.find("@1#sha256:") !=
                  std::string::npos &&
              plan.evidence.front().evidence_id.find("miss009_table") ==
                  std::string::npos &&
              plan.evidence.front().evidence_id.find(kTableUuid) ==
                  std::string::npos,
          "MISS-009 canonical accepted planning evidence missing or unredacted");

  auto missing_operand = request;
  missing_operand.envelope.operands.clear();
  const auto missing = sblr::DispatchSblrOperation(missing_operand);
  Require(!missing.api_result.ok &&
              HasExactEnvelopeRefusal(missing, "SBLR.OPERAND_INVALID"),
          "MISS-009 missing descriptor did not use SBLR.OPERAND_INVALID");
  auto wrong_identity = request;
  wrong_identity.envelope.opcode_code = 0;
  const auto wrong = sblr::DispatchSblrOperation(wrong_identity);
  Require(!wrong.accepted && HasExactIdentityEnvelopeRefusal(wrong),
          "MISS-009 code-zero identity did not use SBLR.OPCODE_INVALID");

  const auto released =
      api::ReleaseEngineBoundImportRowsPlanDescriptorsV1(binder_context);
  Require(released.ok && released.released_row_count == 1,
          "MISS-009 exact plan descriptor release failed");
  Require(kPlanImportPublicAbiProofTarget ==
              "sbsql_sblr_alignment_plan_import_rows_sbps_coordination",
          "MISS-009 public-ABI proof provenance drifted");
  return plan;
}

void VerifyRejectAndCheckpointModels(const api::EngineRequestContext& context) {
  api::EngineNormalizeImportRejectModelRequest reject_request;
  reject_request.context = context;
  reject_request.target_table.uuid.canonical = kTableUuid;
  reject_request.target_table.object_kind = "table";
  reject_request.reject_policy.reject_mode = "reject_row";
  reject_request.reject_policy.reject_limit_rows = 10;
  reject_request.reject_policy.reject_payload_policy = "diagnostic_only";
  reject_request.reject_policy.resume_policy = "fail_closed";
  auto reject = api::EngineNormalizeImportRejectModel(reject_request);
  Require(reject.ok, "MISS-009 reject model normalization failed");
  Require(reject.error_row_schema.schema_version == 1,
          "MISS-009 reject row schema version mismatch");
  Require(HasEvidence(reject, "import_reject_model", "reject_row"),
          "MISS-009 reject model evidence missing");

  api::EngineNormalizeImportCheckpointRequest checkpoint_request;
  checkpoint_request.context = context;
  checkpoint_request.target_table.uuid.canonical = kTableUuid;
  checkpoint_request.target_table.object_kind = "table";
  checkpoint_request.checkpoint_policy.checkpoint_mode = "disabled";
  checkpoint_request.checkpoint_policy.resume_policy = "fail_closed";
  auto checkpoint = api::EngineNormalizeImportCheckpointModel(checkpoint_request);
  Require(checkpoint.ok, "MISS-009 checkpoint model normalization failed");
  Require(!checkpoint.checkpoint_required,
          "MISS-009 disabled checkpoint unexpectedly required checkpointing");
  Require(HasEvidence(checkpoint, "import_checkpoint_model", "disabled"),
          "MISS-009 checkpoint model evidence missing");
}

void VerifyFailFastCopyExecution(const api::EngineRequestContext& context) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_fingerprint = "miss009-copy-fast";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000201", "1", "copy-fast-a"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000202", "2", "copy-fast-b"));
  request.estimated_row_count = request.canonical_rows.size();

  auto executed = api::EngineExecuteImportRows(request);
  Require(executed.ok, "MISS-009 fail-fast import execution failed");
  Require(executed.accepted_rows == 2 && executed.inserted_rows == 2,
          "MISS-009 fail-fast import row counts mismatch");
  Require(HasEvidence(executed, "import_execution", "direct_physical"),
          "MISS-009 fail-fast import did not use direct physical COPY path");
  Require(HasEvidence(executed, "import_execution_delegate", "none"),
          "MISS-009 fail-fast import delegated unexpectedly");
  Require(HasEvidence(executed, "import_plan_consumed", "false"),
          "MISS-009 execution claimed implicit plan consumption");
  Require(HasEvidence(executed,
                      "import_execution_descriptor_revalidated", "true"),
          "MISS-009 execution did not revalidate its import descriptor");
  Require(HasEvidence(executed, "parser_finality_authority", "false"),
          "MISS-009 import allowed parser finality authority");
  Require(HasEvidence(executed, "reference_finality_authority", "false"),
          "MISS-009 import allowed reference finality authority");
}

void VerifyRejectRowExecution(const api::EngineRequestContext& context) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_fingerprint = "miss009-copy-reject";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "reject_row";
  request.import_policy.reject_limit_rows = 10;
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000203", "3", "copy-reject-valid"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000204", "1", "copy-reject-duplicate"));
  request.estimated_row_count = request.canonical_rows.size();

  auto executed = api::EngineExecuteImportRows(request);
  Require(executed.ok, "MISS-009 reject-row import execution failed");
  Require(executed.accepted_rows == 1 && executed.rejected_rows == 1,
          "MISS-009 reject-row import counts mismatch");
  Require(HasEvidence(executed, "import_execution",
                      "delegated_to_dml.insert_rows"),
          "MISS-009 reject-row import did not use row reject path");
  Require(HasEvidence(executed, "import_reject_materialization",
                      "result_shape"),
          "MISS-009 reject-row diagnostics not materialized");
  Require(HasEvidence(executed, "copy_append_reject_fallback", "bisection"),
          "MISS-009 reject-row bisection proof missing");
  Require(HasEvidence(executed, "import_plan_consumed", "false"),
          "MISS-009 reject-row execution claimed implicit plan consumption");
  Require(HasEvidence(executed,
                      "import_execution_descriptor_revalidated", "true"),
          "MISS-009 reject-row execution did not revalidate its descriptor");
  Require(FieldValue(executed, "diagnostic_code", executed.result_shape.rows.size() - 1) ==
              "CLI.CONSTRAINT_UNIQUE_VIOLATION",
          "MISS-009 reject diagnostic code mismatch");
}

void VerifyNativeBinaryRowset(const api::EngineRequestContext& context) {
  api::EngineExecuteNativeBulkIngestRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.checkpoint_policy.resume_policy = "fail_closed";
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000205", "4", "native-bulk-a"));
  request.canonical_rows.push_back(
      Row("019f2900-0000-7000-8000-000000000206", "5", "native-bulk-b"));
  request.estimated_row_count = request.canonical_rows.size();

  auto ingested = api::EngineExecuteNativeBulkIngest(request);
  Require(ingested.ok, "MISS-009 native binary rowset ingest failed");
  Require(ingested.accepted_rows == 2 && ingested.inserted_rows == 2,
          "MISS-009 native binary rowset counts mismatch");
  Require(HasEvidence(ingested, "native_bulk_ingest", "enabled"),
          "MISS-009 native bulk enabled evidence missing");
  Require(HasEvidence(ingested, "native_bulk_ingest_batch_format",
                      "engine_binary_row_batch"),
          "MISS-009 native binary rowset format evidence missing");
  Require(HasEvidence(ingested, "native_bulk_ingest_lane", "direct_physical"),
          "MISS-009 native bulk direct lane evidence missing");

  request.native_bulk_ingest_enabled = false;
  auto refused = api::EngineExecuteNativeBulkIngest(request);
  Require(!refused.ok, "MISS-009 disabled native bulk ingest unexpectedly passed");
  Require(HasDiagnostic(refused, "DML.NATIVE_BULK_INGEST.DISABLED"),
          "MISS-009 disabled native bulk diagnostic missing");
  Require(HasEvidence(refused, "native_bulk_ingest", "disabled"),
          "MISS-009 disabled native bulk evidence missing");
}

void VerifyCatalogArtifactExportImport(const api::EngineRequestContext& context) {
  api::EngineExportCatalogArtifactsRequest export_request;
  export_request.context = context;
  auto exported_before = api::EngineExportCatalogArtifacts(export_request);
  Require(exported_before.ok, "MISS-009 catalog artifact export failed");
  Require(HasEvidence(exported_before, "catalog_artifact_format",
                      "sb.catalog.artifact.v1"),
          "MISS-009 catalog artifact export format evidence missing");
  Require(HasEvidence(exported_before, "git_runtime_authority", "false"),
          "MISS-009 catalog artifact export claimed git runtime authority");

  api::EngineImportCatalogArtifactsRequest import_request;
  import_request.context = context;
  import_request.rows.push_back(ArtifactRow());
  import_request.option_envelopes.push_back("uuid_mode:preserve");
  import_request.option_envelopes.push_back("conflict_policy:reject");
  auto imported = api::EngineImportCatalogArtifacts(import_request);
  Require(imported.ok, "MISS-009 catalog artifact import failed");
  Require(HasEvidence(imported, "catalog_artifact_import_count", "1"),
          "MISS-009 catalog artifact import count evidence missing");
  Require(HasEvidence(imported, "catalog_artifact_imported", kArtifactUuid),
          "MISS-009 catalog artifact imported UUID evidence missing");
  Require(HasEvidence(imported, "git_runtime_authority", "false"),
          "MISS-009 catalog artifact import claimed git runtime authority");

  auto exported_after = api::EngineExportCatalogArtifacts(export_request);
  Require(exported_after.ok, "MISS-009 post-import artifact export failed");
  Require(HasEvidence(exported_after, "catalog_artifact_export_count"),
          "MISS-009 post-import artifact export count evidence missing");
}

struct ParserCase {
  std::string_view sql;
  std::string_view surface_id;
  std::string_view canonical_name;
};

parser::SessionContext ParserSession() {
  parser::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f2900-0000-7000-8000-000000000801";
  session.connection_uuid = "019f2900-0000-7000-8000-000000000802";
  session.database_uuid = kDatabaseUuid;
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 290;
  session.security_policy_epoch = 291;
  session.descriptor_epoch = 292;
  return session;
}

parser::ParserConfig ParserConfigForTest() {
  parser::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsql_miss009_bulk";
  config.parser_uuid = "019f2900-0000-7000-8000-000000000803";
  config.bundle_contract_id = "sbp_sbsql@sbsql-miss-009";
  config.build_id = "sbsql-miss-009-bulk-import-export";
  return config;
}

void VerifyBulkParserRoute(const ParserCase& test_case) {
  const auto session = ParserSession();
  auto cst = parser::BuildCst(std::string(test_case.sql));
  auto ast = parser::BuildAst(cst);
  auto bound = parser::BindAst(ast,
                               cst,
                               ParserConfigForTest(),
                               session,
                               {std::string(kTableUuid), std::string(kSourceUuid)});
  auto envelope = parser::LowerToSblr(bound, cst, session);
  auto verifier = parser::VerifySblrEnvelope(envelope);
  if (cst.messages.has_errors()) std::cerr << parser::RenderMessageVectorSet(cst.messages);
  if (ast.messages.has_errors()) std::cerr << parser::RenderMessageVectorSet(ast.messages);
  if (!bound.bound) std::cerr << parser::RenderMessageVectorSet(bound.messages);
  Require(!cst.messages.has_errors(), "MISS-009 bulk CST failed");
  Require(!ast.messages.has_errors(), "MISS-009 bulk AST failed");
  Require(bound.bound, "MISS-009 bulk bind failed");
  Require(!verifier.admitted && verifier.messages.has_errors(),
          "MISS-009 gated import route did not fail closed");
  Require(envelope.operation_id == "engine.op.diagnostic_refusal",
          "MISS-009 refusal route operation mismatch");
  Require(envelope.engine_api_operation_id == "not_admitted",
          "MISS-009 refusal route engine API mismatch");
  Require(envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL",
          "MISS-009 refusal route opcode mismatch");
  Require(envelope.operation_family == "sblr.dml.operation.v3",
          "MISS-009 refusal route parser family mismatch");
  Require(envelope.surface_key == test_case.canonical_name &&
              envelope.result_shape_key == "diagnostic_vector.v1" &&
              envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              envelope.payload.empty(),
          "MISS-009 refusal route emitted an executable carrier");
  Require(envelope.messages.diagnostics.size() == 1,
          "MISS-009 refusal route did not emit one canonical diagnostic");
  const auto& diagnostic = envelope.messages.diagnostics.front();
  const auto field = [&diagnostic](const std::string_view name) {
    const auto found = std::find_if(
        diagnostic.fields.begin(), diagnostic.fields.end(),
        [name](const auto& item) { return item.name == name; });
    return found == diagnostic.fields.end() ? std::string{} : found->value;
  };
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              field("surface_id") == test_case.surface_id &&
              field("canonical_name") == test_case.canonical_name,
          "MISS-009 refusal diagnostic identity drifted");
  Require(std::find(envelope.required_authority_steps.begin(),
                    envelope.required_authority_steps.end(),
                    "authority.parser.no_executable_sblr") !=
              envelope.required_authority_steps.end() &&
              std::find(envelope.required_authority_steps.begin(),
                        envelope.required_authority_steps.end(),
                        "authority.parser.no_storage_or_finality") !=
                  envelope.required_authority_steps.end(),
          "MISS-009 refusal route lost no-execution authority proof");
}

void VerifyBulkParserRoutes() {
  const ParserCase cases[] = {
      {"LOAD CSV INTO customer FROM source;", "SBSQL-DB993AE8EDBB", "load_data_clause"},
      {"LOAD XML INTO customer FROM source;", "SBSQL-DB993AE8EDBB", "load_data_clause"},
      {"BULK IMPORT JOB customer FROM source;", "SBSQL-DB993AE8EDBB", "load_data_clause"},
      {"INGEST LINE_PROTOCOL INTO customer FROM source;", "SBSQL-DB993AE8EDBB", "load_data_clause"},
  };
  for (const auto& test_case : cases) {
    VerifyBulkParserRoute(test_case);
  }
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  VerifyBulkParserRoutes();
  const auto work = MakeTempDir();
  Require(!work.empty(), "MISS-009 failed to create temp directory");
  const auto database_path = work / "miss009.sbdb";

  const auto created =
      scratchbird::tests::database_lifecycle::CreateCredentialedDatabaseFixture(
          database_path, SB_MISS009_SEED_PACK_ROOT);
  Require(created.ok(), "MISS-009 credentialed fixture database create failed");
  CreateSchemaAndTable(database_path);

  auto context = BeginTransaction(database_path, "201");
  VerifyImportPlanning(context);
  VerifyRejectAndCheckpointModels(context);
  VerifyFailFastCopyExecution(context);
  VerifyRejectRowExecution(context);
  VerifyNativeBinaryRowset(context);
  VerifyCatalogArtifactExportImport(context);
  Commit(context);

  std::error_code cleanup_error;
  std::filesystem::remove_all(work, cleanup_error);
  std::cout << "plan_import_public_abi_proof="
            << kPlanImportPublicAbiProofTarget << '\n';
  return EXIT_SUCCESS;
}
