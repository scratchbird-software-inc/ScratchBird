// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kFamily = "sblr.management.runtime_operation.v3";
constexpr std::string_view kAdmissionFamily = "sblr.database.management.v3";
constexpr std::string_view kAttachOperation = "lifecycle.attach_database";
constexpr std::string_view kAttachOpcode = "SBLR_LIFECYCLE_ATTACH_DATABASE";
constexpr std::string_view kCreateOperation = "lifecycle.create_database";
constexpr std::string_view kCreateOpcode = "SBLR_LIFECYCLE_CREATE_DATABASE";
constexpr std::string_view kDetachOperation = "lifecycle.detach_database";
constexpr std::string_view kDetachOpcode = "SBLR_LIFECYCLE_DETACH_DATABASE";
constexpr std::string_view kDropOperation = "lifecycle.drop_database";
constexpr std::string_view kDropOpcode = "SBLR_LIFECYCLE_DROP_DATABASE";
constexpr std::string_view kEnterMaintenanceOperation = "lifecycle.enter_maintenance";
constexpr std::string_view kEnterMaintenanceOpcode = "SBLR_LIFECYCLE_ENTER_MAINTENANCE";
constexpr std::string_view kForceShutdownOperation = "lifecycle.shutdown_force";
constexpr std::string_view kForceShutdownOpcode = "SBLR_LIFECYCLE_SHUTDOWN_FORCE";
constexpr std::string_view kInspectOperation = "lifecycle.inspect_database";
constexpr std::string_view kInspectOpcode = "SBLR_LIFECYCLE_INSPECT_DATABASE";
constexpr std::string_view kOpenOperation = "lifecycle.open_database";
constexpr std::string_view kOpenOpcode = "SBLR_LIFECYCLE_OPEN_DATABASE";
constexpr std::string_view kVerifyOperation = "lifecycle.verify_database";
constexpr std::string_view kVerifyOpcode = "SBLR_LIFECYCLE_VERIFY_DATABASE";
constexpr std::string_view kRepairOperation = "lifecycle.repair_database";
constexpr std::string_view kRepairOpcode = "SBLR_LIFECYCLE_REPAIR_DATABASE";
constexpr std::string_view kShutdownAcknowledgeOperation =
    "lifecycle.shutdown_acknowledge";
constexpr std::string_view kShutdownAcknowledgeOpcode =
    "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE";
constexpr std::string_view kShutdownOperation = "lifecycle.shutdown_database";
constexpr std::string_view kShutdownOpcode = "SBLR_LIFECYCLE_SHUTDOWN_DATABASE";
constexpr std::string_view kDatabasePath =
    "/tmp/sbsql_database_lifecycle_exact_route_conformance.sbdb";

struct RegistryRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
};

constexpr std::array<RegistryRowEvidence, 9> kRows{{
    {"SBSQL-1F38A5CF36B0", "alter_database_action", "grammar_production"},
    {"SBSQL-796DE5C0B192", "alter_database_extra", "grammar_production"},
    {"SBSQL-EB95D772BD63", "create_database_stmt", "grammar_production"},
    {"SBSQL-80C5BA542433", "attach_database_stmt", "grammar_production"},
    {"SBSQL-A3F3AF6910F9", "database_name", "grammar_production"},
    {"SBSQL-F4F4216A8C8A", "repair_options", "grammar_production"},
    {"SBSQL-5B1C5630A433", "use_database_alias", "canonical_surface"},
    {"SBSQL-0E7563017DCD", "verify_options", "grammar_production"},
    {"SBSQL-F9BE1FC733F6", "maintenance_stmt", "grammar_production"},
}};

struct LifecycleRouteCase {
  std::string_view sql;
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view mapping_key;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view required_right;
  bool requires_security_context;
  bool database_uuid_generated_by_engine;
  std::vector<std::string_view> row_surface_ids;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

void DumpMessages(std::string_view phase,
                  std::string_view sql,
                  const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << phase << ':' << sql << ':'
              << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind,
                          std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) return true;
  }
  return false;
}

bool ApiResultHasDiagnostic(const api::EngineApiResult& result,
                            std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool ApiResultHasField(const api::EngineApiResult& result,
                       std::string_view name,
                       std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name && field.second.encoded_value == value) return true;
    }
  }
  return false;
}

std::string ApiResultFieldValue(const api::EngineApiResult& result,
                                std::string_view name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == name) return field.second.encoded_value;
    }
  }
  return {};
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000ef1001";
  session.connection_uuid = "019f0000-0000-7000-8000-000000ef1002";
  session.database_uuid = "019f0000-0000-7000-8000-000000ef1003";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 71;
  session.security_policy_epoch = 72;
  session.descriptor_epoch = 73;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000ef1004";
  config.bundle_contract_id = "sbp_sbsql@database-lifecycle-route-test";
  config.build_id = "sbsql-database-lifecycle-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineRequestContext BaseEngineContext(std::string request_id) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = std::string(kDatabasePath);
  context.security_context_present = true;
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000ef2001";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000ef2002";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000ef2003";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000ef2004";
  context.cluster_uuid.canonical = "019f0000-0000-7000-8000-000000ef2005";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000ef2006";
  context.catalog_generation_id = 71;
  context.security_epoch = 72;
  context.resource_epoch = 73;
  context.name_resolution_epoch = 74;
  context.trace_tags.push_back("right:LIFECYCLE_DATABASE_ATTACH");
  return context;
}

void RemoveEngineFiles() {
  const std::filesystem::path path(kDatabasePath);
  std::filesystem::remove(path);
  for (const auto suffix : {
           ".sb.crud_events",
           ".sb.mga_row_versions",
           ".sb.mga_relation_metadata",
           ".sb.mga_index_entries",
           ".sb.mga_relation_descriptors",
           ".sb.mga_large_values",
           ".sb.mga_savepoints",
           ".sb.api_events",
           ".sb.catalog_object_events",
           ".sb.domain_events",
           ".sb.drop_evidence",
           ".sb.local_password_auth",
           ".dirty.manifest",
           ".recovery.evidence",
       }) {
    std::filesystem::remove(std::filesystem::path(std::string(kDatabasePath) + suffix));
  }
}

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "database lifecycle registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "database lifecycle registry canonical name mismatch");
    Require(registry_row->surface_kind == row.surface_kind,
            "database lifecycle registry surface kind mismatch");
    Require(registry_row->source_status == "native_now",
            "database lifecycle registry source status mismatch");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "database lifecycle registry cluster scope mismatch");
  }
}

void RequireParserLoweringAndAdmission(const LifecycleRouteCase& route) {
  const auto artifacts = RunPipeline(route.sql);
  DumpMessages("cst", route.sql, artifacts.cst.messages);
  DumpMessages("ast", route.sql, artifacts.ast.messages);
  DumpMessages("bind", route.sql, artifacts.bound.messages);
  DumpMessages("lower", route.sql, artifacts.envelope.messages);
  Require(!artifacts.cst.messages.has_errors(), "database lifecycle CST emitted errors");
  Require(!artifacts.ast.messages.has_errors(), "database lifecycle AST emitted errors");
  if (!route.surface_id.empty()) {
    Require(artifacts.ast.statement_surface_id == route.surface_id,
            "database lifecycle AST surface id mismatch");
    Require(artifacts.ast.statement_surface_name == route.canonical_name,
            "database lifecycle AST surface name mismatch");
  }
  Require(artifacts.bound.bound, "database lifecycle route did not bind");
  Require(artifacts.envelope.lifecycle_mapping,
          "database lifecycle route did not lower through lifecycle mapping");
  Require(artifacts.envelope.operation_id == route.operation_id,
          "database lifecycle operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == route.operation_id,
          "database lifecycle engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == route.opcode,
          "database lifecycle SBLR opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "database lifecycle operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "database lifecycle operation key mismatch");
  Require(artifacts.envelope.engine_api_function == route.engine_api_function,
          "database lifecycle engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_rights, route.required_right),
          "database lifecycle required right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.lifecycle_api_required"),
          "database lifecycle engine lifecycle authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_lifecycle_evidence_required"),
          "database lifecycle MGA evidence authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "database lifecycle parser finality guard missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.donor_filesystem_effects_forbidden"),
          "database lifecycle donor filesystem guard missing");
  if (route.requires_security_context) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.server.security_policy_context_required"),
            "database lifecycle security authority missing");
  }
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"mapping_key\":\"") +
                       std::string(route.mapping_key) + "\""),
          "database lifecycle payload mapping key missing");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"requires_security_context\":") +
                       (route.requires_security_context ? "true" : "false")),
          "database lifecycle payload security requirement mismatch");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"database_uuid_generated_by_engine\":") +
                       (route.database_uuid_generated_by_engine ? "true" : "false")),
          "database lifecycle payload database UUID-generation evidence mismatch");
  for (const auto row_surface_id : route.row_surface_ids) {
    Require(Contains(artifacts.envelope.payload, row_surface_id),
            "database lifecycle payload row surface id missing");
  }
  Require(!Contains(artifacts.envelope.payload, std::string(route.sql)),
          "database lifecycle payload embedded source SQL");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\"") &&
              !Contains(artifacts.envelope.payload, "\"sql_text\":"),
          "database lifecycle payload embedded source text fields");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "database lifecycle payload carried WAL/recovery authority");
  Require(artifacts.verifier.admitted,
          "database lifecycle verifier rejected exact route");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "server admission rejected database lifecycle route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for lifecycle route");
  Require(admission.operation_id == route.operation_id,
          "server admission lifecycle operation id mismatch");
  Require(admission.operation_family == kAdmissionFamily,
          "server admission lifecycle family mismatch");
}

sblr::SblrOperationEnvelope LifecycleEnvelope(std::string operation_id,
                                              std::string opcode,
                                              bool requires_security_context) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "trace.database_lifecycle.exact_route");
  envelope.requires_security_context = requires_security_context;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch() {
  RemoveEngineFiles();

  api::EngineApiRequest create_api_request;
  create_api_request.option_envelopes.push_back("allow_minimal_resource_bootstrap:true");
  create_api_request.option_envelopes.push_back("page_size:16384");
  const sblr::SblrDispatchRequest create_request{
      BaseEngineContext("sbsql-database-lifecycle-create-route"),
      LifecycleEnvelope(std::string(kCreateOperation),
                        std::string(kCreateOpcode),
                        false),
      create_api_request};
  const auto created = sblr::DispatchSblrOperation(create_request);
  for (const auto& diagnostic : created.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(created.envelope_validated, "lifecycle.create_database envelope did not validate");
  Require(created.accepted, "lifecycle.create_database dispatch was not accepted");
  Require(created.dispatched_to_api, "lifecycle.create_database did not dispatch to API");
  Require(created.api_result.ok, "lifecycle.create_database returned a diagnostic");
  Require(created.api_result.operation_id == kCreateOperation,
          "lifecycle.create_database returned wrong operation id");
  Require(created.api_result.primary_object.object_kind == "database",
          "lifecycle.create_database returned wrong primary object kind");
  Require(ApiResultHasEvidence(created.api_result, "engine_lifecycle", "created"),
          "lifecycle.create_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(created.api_result, "database_file", "created"),
          "lifecycle.create_database missing database file evidence");
  Require(ApiResultHasField(created.api_result, "lifecycle_state", "created"),
          "lifecycle.create_database missing lifecycle_state row");

  auto open_context = BaseEngineContext("sbsql-database-lifecycle-open-route");
  open_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  const sblr::SblrDispatchRequest open_request{
      open_context,
      LifecycleEnvelope(std::string(kOpenOperation),
                        std::string(kOpenOpcode),
                        false),
      api::EngineApiRequest{}};
  const auto opened = sblr::DispatchSblrOperation(open_request);
  for (const auto& diagnostic : opened.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(opened.envelope_validated, "lifecycle.open_database envelope did not validate");
  Require(opened.accepted, "lifecycle.open_database dispatch was not accepted");
  Require(opened.dispatched_to_api, "lifecycle.open_database did not dispatch to API");
  Require(opened.api_result.ok, "lifecycle.open_database returned a diagnostic");
  Require(opened.api_result.operation_id == kOpenOperation,
          "lifecycle.open_database returned wrong operation id");
  Require(ApiResultHasEvidence(opened.api_result, "engine_lifecycle", "opened"),
          "lifecycle.open_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(opened.api_result, "mga_lifecycle_evidence",
                               "transaction_inventory_recorded"),
          "lifecycle.open_database missing MGA lifecycle evidence");
  Require(ApiResultHasField(opened.api_result, "lifecycle_state", "opened"),
          "lifecycle.open_database missing lifecycle_state row");

  auto attach_context = BaseEngineContext("sbsql-database-lifecycle-attach-route");
  attach_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest attach_api_request;
  attach_api_request.option_envelopes.push_back("attach");
  const sblr::SblrDispatchRequest attach_request{
      attach_context,
      LifecycleEnvelope(std::string(kAttachOperation),
                        std::string(kAttachOpcode),
                        true),
      attach_api_request};
  const auto attached = sblr::DispatchSblrOperation(attach_request);
  for (const auto& diagnostic : attached.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(attached.envelope_validated, "lifecycle.attach_database envelope did not validate");
  Require(attached.accepted, "lifecycle.attach_database dispatch was not accepted");
  Require(attached.dispatched_to_api, "lifecycle.attach_database did not dispatch to API");
  Require(attached.api_result.ok, "lifecycle.attach_database returned a diagnostic");
  Require(attached.api_result.operation_id == kAttachOperation,
          "lifecycle.attach_database returned wrong operation id");
  Require(ApiResultHasEvidence(attached.api_result, "engine_lifecycle", "attached"),
          "lifecycle.attach_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(attached.api_result, "mga_lifecycle_evidence",
                               "transaction_inventory_recorded"),
          "lifecycle.attach_database missing MGA lifecycle evidence");
  Require(ApiResultHasField(attached.api_result, "lifecycle_state", "attached"),
          "lifecycle.attach_database missing lifecycle_state row");

  auto maintenance_context = BaseEngineContext("sbsql-database-lifecycle-maintenance-route");
  maintenance_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest maintenance_api_request;
  maintenance_api_request.option_envelopes.push_back("mode:maintenance");
  const sblr::SblrDispatchRequest maintenance_request{
      maintenance_context,
      LifecycleEnvelope(std::string(kEnterMaintenanceOperation),
                        std::string(kEnterMaintenanceOpcode),
                        true),
      maintenance_api_request};
  const auto maintenance = sblr::DispatchSblrOperation(maintenance_request);
  for (const auto& diagnostic : maintenance.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(maintenance.envelope_validated,
          "lifecycle.enter_maintenance envelope did not validate");
  Require(maintenance.accepted,
          "lifecycle.enter_maintenance dispatch was not accepted");
  Require(maintenance.dispatched_to_api,
          "lifecycle.enter_maintenance did not dispatch to API");
  Require(maintenance.api_result.ok,
          "lifecycle.enter_maintenance returned a diagnostic");
  Require(maintenance.api_result.operation_id == kEnterMaintenanceOperation,
          "lifecycle.enter_maintenance returned wrong operation id");
  Require(ApiResultHasEvidence(maintenance.api_result, "engine_lifecycle", "maintenance"),
          "lifecycle.enter_maintenance missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(maintenance.api_result, "mga_lifecycle_evidence",
                               "transaction_inventory_recorded"),
          "lifecycle.enter_maintenance missing MGA lifecycle evidence");
  Require(ApiResultHasField(maintenance.api_result, "lifecycle_state", "maintenance"),
          "lifecycle.enter_maintenance missing lifecycle_state row");

  auto verify_context = BaseEngineContext("sbsql-database-lifecycle-verify-route");
  verify_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest verify_api_request;
  verify_api_request.option_envelopes.push_back("mode:maintenance");
  const sblr::SblrDispatchRequest verify_request{
      verify_context,
      LifecycleEnvelope(std::string(kVerifyOperation),
                        std::string(kVerifyOpcode),
                        true),
      verify_api_request};
  const auto verified = sblr::DispatchSblrOperation(verify_request);
  for (const auto& diagnostic : verified.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(verified.envelope_validated,
          "lifecycle.verify_database envelope did not validate");
  Require(verified.accepted,
          "lifecycle.verify_database dispatch was not accepted");
  Require(verified.dispatched_to_api,
          "lifecycle.verify_database did not dispatch to API");
  Require(verified.api_result.ok,
          "lifecycle.verify_database returned a diagnostic");
  Require(verified.api_result.operation_id == kVerifyOperation,
          "lifecycle.verify_database returned wrong operation id");
  Require(ApiResultHasEvidence(verified.api_result, "engine_lifecycle", "verified"),
          "lifecycle.verify_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(verified.api_result, "lifecycle_verify",
                               "storage_open_classification_passed"),
          "lifecycle.verify_database missing verify evidence");
  Require(ApiResultHasField(verified.api_result, "verification_result", "passed"),
          "lifecycle.verify_database missing verification result row");

  const auto filespace_uuid = ApiResultFieldValue(created.api_result, "filespace_uuid");
  Require(!filespace_uuid.empty(), "lifecycle.create_database missing filespace UUID row");
  auto repair_context = BaseEngineContext("sbsql-database-lifecycle-repair-route");
  repair_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest repair_api_request;
  repair_api_request.option_envelopes.push_back("mode:maintenance");
  repair_api_request.option_envelopes.push_back("repair_plan_id:record_verified_repair_evidence");
  repair_api_request.option_envelopes.push_back("repair_admission_proven:true");
  repair_api_request.option_envelopes.push_back("allow_repair:true");
  repair_api_request.option_envelopes.push_back(
      std::string("expected_database_uuid:") + created.api_result.primary_object.uuid.canonical);
  repair_api_request.option_envelopes.push_back(
      std::string("expected_filespace_uuid:") + filespace_uuid);
  const sblr::SblrDispatchRequest repair_request{
      repair_context,
      LifecycleEnvelope(std::string(kRepairOperation),
                        std::string(kRepairOpcode),
                        true),
      repair_api_request};
  const auto repaired = sblr::DispatchSblrOperation(repair_request);
  for (const auto& diagnostic : repaired.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(repaired.envelope_validated,
          "lifecycle.repair_database envelope did not validate");
  Require(repaired.accepted,
          "lifecycle.repair_database dispatch was not accepted");
  Require(repaired.dispatched_to_api,
          "lifecycle.repair_database did not dispatch to API");
  Require(repaired.api_result.ok,
          "lifecycle.repair_database returned a diagnostic");
  Require(repaired.api_result.operation_id == kRepairOperation,
          "lifecycle.repair_database returned wrong operation id");
  Require(ApiResultHasEvidence(repaired.api_result, "engine_lifecycle",
                               "repair_completed"),
          "lifecycle.repair_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(repaired.api_result, "lifecycle_repair",
                               "storage_repair_evidence_recorded"),
          "lifecycle.repair_database missing repair evidence");
  Require(ApiResultHasEvidence(repaired.api_result, "mga_lifecycle_evidence",
                               "repair_transaction_recorded"),
          "lifecycle.repair_database missing MGA repair evidence");
  Require(ApiResultHasField(repaired.api_result, "repair_result", "completed"),
          "lifecycle.repair_database missing repair result row");

  auto inspect_context = BaseEngineContext("sbsql-database-lifecycle-inspect-route");
  inspect_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  const sblr::SblrDispatchRequest inspect_request{
      inspect_context,
      LifecycleEnvelope(std::string(kInspectOperation),
                        std::string(kInspectOpcode),
                        true),
      api::EngineApiRequest{}};
  const auto inspected = sblr::DispatchSblrOperation(inspect_request);
  for (const auto& diagnostic : inspected.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(inspected.envelope_validated,
          "lifecycle.inspect_database envelope did not validate");
  Require(inspected.accepted,
          "lifecycle.inspect_database dispatch was not accepted");
  Require(inspected.dispatched_to_api,
          "lifecycle.inspect_database did not dispatch to API");
  Require(inspected.api_result.ok,
          "lifecycle.inspect_database returned a diagnostic");
  Require(inspected.api_result.operation_id == kInspectOperation,
          "lifecycle.inspect_database returned wrong operation id");
  Require(ApiResultHasEvidence(inspected.api_result, "engine_lifecycle", "inspected"),
          "lifecycle.inspect_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(inspected.api_result, "database_file",
                               "read_only_open_classified"),
          "lifecycle.inspect_database missing read-only file classification evidence");
  Require(ApiResultHasField(inspected.api_result, "lifecycle_state", "inspected"),
          "lifecycle.inspect_database missing lifecycle_state row");

  auto detach_context = BaseEngineContext("sbsql-database-lifecycle-detach-route");
  detach_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest detach_api_request;
  detach_api_request.option_envelopes.push_back("detach");
  const sblr::SblrDispatchRequest detach_request{
      detach_context,
      LifecycleEnvelope(std::string(kDetachOperation),
                        std::string(kDetachOpcode),
                        true),
      detach_api_request};
  const auto detached = sblr::DispatchSblrOperation(detach_request);
  for (const auto& diagnostic : detached.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(detached.envelope_validated,
          "lifecycle.detach_database envelope did not validate");
  Require(detached.accepted,
          "lifecycle.detach_database dispatch was not accepted");
  Require(detached.dispatched_to_api,
          "lifecycle.detach_database did not dispatch to API");
  Require(detached.api_result.ok,
          "lifecycle.detach_database returned a diagnostic");
  Require(detached.api_result.operation_id == kDetachOperation,
          "lifecycle.detach_database returned wrong operation id");
  Require(ApiResultHasEvidence(detached.api_result, "engine_lifecycle", "detached"),
          "lifecycle.detach_database missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(detached.api_result, "lifecycle_metric",
                               "transition_event_emitted"),
          "lifecycle.detach_database missing transition metric evidence");

  auto force_context = BaseEngineContext("sbsql-database-lifecycle-force-shutdown-route");
  force_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest force_api_request;
  force_api_request.option_envelopes.push_back(
      "force_termination_policy_uuid:019f0000-0000-7000-8000-000000ef20f1");
  force_api_request.option_envelopes.push_back("association_scope_proven:true");
  force_api_request.option_envelopes.push_back("recovery_evidence_preserved:true");
  const sblr::SblrDispatchRequest force_request{
      force_context,
      LifecycleEnvelope(std::string(kForceShutdownOperation),
                        std::string(kForceShutdownOpcode),
                        true),
      force_api_request};
  const auto forced = sblr::DispatchSblrOperation(force_request);
  for (const auto& diagnostic : forced.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(forced.envelope_validated,
          "lifecycle.shutdown_force envelope did not validate");
  Require(forced.accepted,
          "lifecycle.shutdown_force dispatch was not accepted");
  Require(forced.dispatched_to_api,
          "lifecycle.shutdown_force did not dispatch to API");
  Require(forced.api_result.ok,
          "lifecycle.shutdown_force returned a diagnostic");
  Require(forced.api_result.operation_id == kForceShutdownOperation,
          "lifecycle.shutdown_force returned wrong operation id");
  Require(ApiResultHasEvidence(forced.api_result, "engine_lifecycle",
                               "shutdown_force_evidence_recorded"),
          "lifecycle.shutdown_force missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(forced.api_result, "mga_recovery_evidence",
                               "preserved"),
          "lifecycle.shutdown_force missing MGA recovery evidence");

  auto acknowledge_context =
      BaseEngineContext("sbsql-database-lifecycle-ack-shutdown-route");
  acknowledge_context.database_uuid.canonical =
      created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest acknowledge_api_request;
  acknowledge_api_request.option_envelopes.push_back("acknowledger_kind:server");
  acknowledge_api_request.option_envelopes.push_back(
      "acknowledger_uuid:019f0000-0000-7000-8000-000000ef20a1");
  acknowledge_api_request.option_envelopes.push_back("acknowledgement_generation:1");
  acknowledge_api_request.option_envelopes.push_back(
      "acknowledgement_state:acknowledged");
  const sblr::SblrDispatchRequest acknowledge_request{
      acknowledge_context,
      LifecycleEnvelope(std::string(kShutdownAcknowledgeOperation),
                        std::string(kShutdownAcknowledgeOpcode),
                        true),
      acknowledge_api_request};
  const auto acknowledged = sblr::DispatchSblrOperation(acknowledge_request);
  for (const auto& diagnostic : acknowledged.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(acknowledged.envelope_validated,
          "lifecycle.shutdown_acknowledge envelope did not validate");
  Require(acknowledged.accepted,
          "lifecycle.shutdown_acknowledge dispatch was not accepted");
  Require(acknowledged.dispatched_to_api,
          "lifecycle.shutdown_acknowledge did not dispatch to API");
  Require(acknowledged.api_result.ok,
          "lifecycle.shutdown_acknowledge returned a diagnostic");
  Require(acknowledged.api_result.operation_id == kShutdownAcknowledgeOperation,
          "lifecycle.shutdown_acknowledge returned wrong operation id");
  Require(ApiResultHasEvidence(acknowledged.api_result, "engine_lifecycle",
                               "shutdown_acknowledged"),
          "lifecycle.shutdown_acknowledge missing engine lifecycle evidence");
  Require(ApiResultHasEvidence(acknowledged.api_result, "shutdown_acknowledgement",
                               "019f0000-0000-7000-8000-000000ef20a1"),
          "lifecycle.shutdown_acknowledge missing acknowledgement evidence");

  auto shutdown_context = BaseEngineContext("sbsql-database-lifecycle-shutdown-route");
  shutdown_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest shutdown_api_request;
  shutdown_api_request.option_envelopes.push_back("shutdown");
  const sblr::SblrDispatchRequest shutdown_request{
      shutdown_context,
      LifecycleEnvelope(std::string(kShutdownOperation),
                        std::string(kShutdownOpcode),
                        true),
      shutdown_api_request};
  const auto shutdown = sblr::DispatchSblrOperation(shutdown_request);
  for (const auto& diagnostic : shutdown.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(shutdown.envelope_validated,
          "lifecycle.shutdown_database envelope did not validate");
  Require(shutdown.accepted,
          "lifecycle.shutdown_database dispatch was not accepted");
  Require(shutdown.dispatched_to_api,
          "lifecycle.shutdown_database did not dispatch to API");
  Require(shutdown.api_result.ok,
          "lifecycle.shutdown_database returned a diagnostic");
  Require(shutdown.api_result.operation_id == kShutdownOperation,
          "lifecycle.shutdown_database returned wrong operation id");
  Require(ApiResultHasEvidence(shutdown.api_result, "engine_lifecycle",
                               "shutdown_clean"),
          "lifecycle.shutdown_database missing clean shutdown evidence");
  Require(ApiResultHasField(shutdown.api_result, "lifecycle_state", "shutdown_clean"),
          "lifecycle.shutdown_database missing lifecycle_state row");
  Require(ApiResultHasField(shutdown.api_result, "clean_shutdown", "true"),
          "lifecycle.shutdown_database missing clean shutdown row");

  auto refused_drop_context =
      BaseEngineContext("sbsql-database-lifecycle-drop-refusal-route");
  refused_drop_context.database_uuid.canonical =
      created.api_result.primary_object.uuid.canonical;
  const sblr::SblrDispatchRequest refused_drop_request{
      refused_drop_context,
      LifecycleEnvelope(std::string(kDropOperation),
                        std::string(kDropOpcode),
                        true),
      api::EngineApiRequest{}};
  const auto refused_drop = sblr::DispatchSblrOperation(refused_drop_request);
  for (const auto& diagnostic : refused_drop.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(refused_drop.envelope_validated,
          "lifecycle.drop_database refusal envelope did not validate");
  Require(refused_drop.accepted,
          "lifecycle.drop_database refusal dispatch was not accepted");
  Require(refused_drop.dispatched_to_api,
          "lifecycle.drop_database refusal did not dispatch to API");
  Require(!refused_drop.api_result.ok,
          "lifecycle.drop_database admitted missing safety preconditions");
  Require(ApiResultHasDiagnostic(
              refused_drop.api_result,
              "SB_ENGINE_API_LIFECYCLE_DROP_PRECONDITIONS_NOT_SATISFIED"),
          "lifecycle.drop_database missing safety-precondition refusal diagnostic");

  auto drop_context = BaseEngineContext("sbsql-database-lifecycle-drop-route");
  drop_context.database_uuid.canonical = created.api_result.primary_object.uuid.canonical;
  api::EngineApiRequest drop_api_request;
  drop_api_request.option_envelopes.push_back("drop_mode:logical");
  drop_api_request.option_envelopes.push_back("drop_safety_preconditions:true");
  drop_api_request.option_envelopes.push_back("session_drain_complete:true");
  drop_api_request.option_envelopes.push_back("ownership_release_verified:true");
  drop_api_request.option_envelopes.push_back("retention_policy_satisfied:true");
  drop_api_request.option_envelopes.push_back("backup_coverage_verified:true");
  drop_api_request.option_envelopes.push_back("legal_hold_clear:true");
  drop_api_request.option_envelopes.push_back(
      std::string("expected_database_uuid:") + created.api_result.primary_object.uuid.canonical);
  drop_api_request.option_envelopes.push_back(
      std::string("expected_filespace_uuid:") + filespace_uuid);
  const sblr::SblrDispatchRequest drop_request{
      drop_context,
      LifecycleEnvelope(std::string(kDropOperation),
                        std::string(kDropOpcode),
                        true),
      drop_api_request};
  const auto dropped = sblr::DispatchSblrOperation(drop_request);
  for (const auto& diagnostic : dropped.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Require(dropped.envelope_validated,
          "lifecycle.drop_database envelope did not validate");
  Require(dropped.accepted,
          "lifecycle.drop_database dispatch was not accepted");
  Require(dropped.dispatched_to_api,
          "lifecycle.drop_database did not dispatch to API");
  Require(dropped.api_result.ok,
          "lifecycle.drop_database returned a diagnostic");
  Require(dropped.api_result.operation_id == kDropOperation,
          "lifecycle.drop_database returned wrong operation id");
  Require(ApiResultHasEvidence(dropped.api_result, "engine_lifecycle",
                               "drop_evidence_recorded"),
          "lifecycle.drop_database missing drop evidence");
  Require(ApiResultHasEvidence(dropped.api_result, "database_file", "preserved"),
          "lifecycle.drop_database did not preserve file for logical drop");
  Require(ApiResultHasField(dropped.api_result, "lifecycle_state",
                            "drop_evidence_recorded"),
          "lifecycle.drop_database missing lifecycle_state row");
  Require(ApiResultHasField(dropped.api_result, "storage_file_deleted", "false"),
          "lifecycle.drop_database logical mode deleted storage");
  Require(std::filesystem::exists(std::filesystem::path(kDatabasePath)),
          "lifecycle.drop_database logical mode removed database file");
  Require(std::filesystem::exists(
              std::filesystem::path(std::string(kDatabasePath) + ".sb.drop_evidence")),
          "lifecycle.drop_database missing drop evidence sidecar");
}

void RequireRoutes() {
  const std::array<LifecycleRouteCase, 15> routes{{
      {"CREATE DATABASE qa_lifecycle",
       "SBSQL-EB95D772BD63",
       "create_database_stmt",
       "sbsql.lifecycle.create_database",
       kCreateOperation,
       kCreateOpcode,
       "EngineCreateLifecycle",
       "right.lifecycle_create",
       false,
       true,
       {"SBSQL-EB95D772BD63"}},
      {"OPEN DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.open_database",
       kOpenOperation,
       kOpenOpcode,
       "EngineOpenLifecycle",
       "right.lifecycle_open",
       false,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"ATTACH DATABASE '/tmp/sbsql_database_lifecycle_exact_route_conformance.sbdb' AS qa_lifecycle",
       "SBSQL-80C5BA542433",
       "attach_database_stmt",
       "sbsql.lifecycle.attach_database",
       kAttachOperation,
       kAttachOpcode,
       "EngineAttachLifecycle",
       "right.lifecycle_attach",
       true,
       false,
       {"SBSQL-80C5BA542433", "SBSQL-A3F3AF6910F9"}},
      {"USE qa_lifecycle",
       "SBSQL-5B1C5630A433",
       "use_database_alias",
       "sbsql.lifecycle.use_database_alias",
       kAttachOperation,
       kAttachOpcode,
       "EngineAttachLifecycle",
       "right.lifecycle_attach",
       true,
       false,
       {"SBSQL-5B1C5630A433", "SBSQL-A3F3AF6910F9"}},
      {"INSPECT DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.inspect_database",
       kInspectOperation,
       kInspectOpcode,
       "EngineInspectLifecycle",
       "right.lifecycle_inspect",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"DIAGNOSE DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.inspect_database",
       kInspectOperation,
       kInspectOpcode,
       "EngineInspectLifecycle",
       "right.lifecycle_inspect",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"ALTER DATABASE qa_lifecycle SET MAINTENANCE WITH EVIDENCE",
       "SBSQL-1F38A5CF36B0",
       "alter_database_action",
       "sbsql.lifecycle.enter_maintenance",
       kEnterMaintenanceOperation,
       kEnterMaintenanceOpcode,
       "EngineEnterMaintenanceLifecycle",
       "right.lifecycle_maintenance",
       true,
       false,
       {"SBSQL-1F38A5CF36B0", "SBSQL-796DE5C0B192", "SBSQL-F9BE1FC733F6"}},
      {"MAINTENANCE DATABASE qa_lifecycle",
       "SBSQL-F9BE1FC733F6",
       "maintenance_stmt",
       "sbsql.lifecycle.enter_maintenance",
       kEnterMaintenanceOperation,
       kEnterMaintenanceOpcode,
       "EngineEnterMaintenanceLifecycle",
       "right.lifecycle_maintenance",
       true,
       false,
       {"SBSQL-F9BE1FC733F6"}},
      {"VERIFY DATABASE qa_lifecycle WITH CHECKSUM",
       "SBSQL-0E7563017DCD",
       "verify_options",
       "sbsql.lifecycle.verify_database",
       kVerifyOperation,
       kVerifyOpcode,
       "EngineVerifyLifecycle",
       "right.lifecycle_verify",
       true,
       false,
       {"SBSQL-0E7563017DCD"}},
      {"REPAIR DATABASE qa_lifecycle WITH PLAN record_verified_repair_evidence",
       "SBSQL-F4F4216A8C8A",
       "repair_options",
       "sbsql.lifecycle.repair_database",
       kRepairOperation,
       kRepairOpcode,
       "EngineRepairLifecycle",
       "right.lifecycle_repair",
       true,
       false,
       {"SBSQL-F4F4216A8C8A"}},
      {"DETACH DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.detach_database",
       kDetachOperation,
       kDetachOpcode,
       "EngineDetachLifecycle",
       "right.lifecycle_detach",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"FORCE SHUTDOWN DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.shutdown_force",
       kForceShutdownOperation,
       kForceShutdownOpcode,
       "EngineForceShutdownLifecycle",
       "right.lifecycle_force_shutdown",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"ACKNOWLEDGE SHUTDOWN DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.shutdown_acknowledge",
       kShutdownAcknowledgeOperation,
       kShutdownAcknowledgeOpcode,
       "EngineAcknowledgeShutdownLifecycle",
       "right.lifecycle_shutdown_acknowledge",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"SHUTDOWN DATABASE qa_lifecycle",
       "",
       "",
       "sbsql.lifecycle.shutdown_database",
       kShutdownOperation,
       kShutdownOpcode,
       "EngineShutdownLifecycle",
       "right.lifecycle_shutdown",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
      {"DROP DATABASE qa_lifecycle LOGICAL",
       "",
       "",
       "sbsql.lifecycle.drop_database",
       kDropOperation,
       kDropOpcode,
       "EngineDropLifecycle",
       "right.lifecycle_drop",
       true,
       false,
       {"SBSQL-A3F3AF6910F9"}},
  }};
  for (const auto& route : routes) {
    RequireParserLoweringAndAdmission(route);
  }
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  RequireRoutes();
  RequireEngineDispatch();
  RemoveEngineFiles();
  return EXIT_SUCCESS;
}
