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
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct B003Row {
  std::string_view audit_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  std::string_view result_shape;
  std::string_view engine_api_function;
  std::string_view authority_step;
  bool requires_transaction_context;
  bool engine_api_command_route;
};

constexpr std::array<B003Row, 46> kRows{{
    {"AUDIT-0383", "ENGINE AGENT REQUEST PAGE PREALLOCATION", "agents.request_page_preallocation", "SBLR_AGENT_REQUEST_PAGE_PREALLOCATION", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineRequestPagePreallocation", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0384", "ENGINE AGENT REQUEST PAGE RELOCATION", "agents.request_page_relocation", "SBLR_AGENT_REQUEST_PAGE_RELOCATION", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineRequestPageRelocation", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0385", "ENGINE AGENT REQUEST FILESPACE GROWTH", "agents.request_filespace_growth", "SBLR_AGENT_REQUEST_FILESPACE_GROWTH", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineRequestFilespaceGrowth", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0386", "ENGINE AGENT NOTIFY FILESPACE SHRINK READINESS", "agents.notify_filespace_shrink_readiness", "SBLR_AGENT_NOTIFY_FILESPACE_SHRINK_READINESS", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineNotifyFilespaceShrinkReadiness", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0387", "ENGINE AGENT REQUEST INDEX DELTA MERGE", "agents.request_index_delta_merge", "SBLR_AGENT_REQUEST_INDEX_DELTA_MERGE", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineRequestIndexDeltaMerge", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0388", "ENGINE AGENT REQUEST INDEX REBUILD", "agents.request_index_rebuild_or_shadow_build", "SBLR_AGENT_REQUEST_INDEX_REBUILD_OR_SHADOW_BUILD", "sblr.management.runtime_operation.v3", "result.shape.agent_hook_status", "EngineRequestIndexRebuildOrShadowBuild", "authority.engine.agent_management_api_required", true, true},
    {"AUDIT-0390", "EXPORT CATALOG ARTIFACT", "artifact.export_catalog", "SBLR_ARTIFACT_EXPORT_CATALOG", "sblr.catalog.mutation.v3", "result.shape.catalog_artifact_rows", "EngineExportCatalogArtifacts", "authority.engine.catalog_artifact_api_required", true, true},
    {"AUDIT-0391", "IMPORT CATALOG ARTIFACT", "artifact.import_catalog", "SBLR_ARTIFACT_IMPORT_CATALOG", "sblr.catalog.mutation.v3", "result.shape.catalog_artifact_status", "EngineImportCatalogArtifacts", "authority.engine.catalog_artifact_api_required", true, true},
    {"AUDIT-0391A", "EXPORT EXTERNAL GIT CATALOG SNAPSHOT", "artifact.external_git.export_snapshot", "SBLR_ARTIFACT_EXTERNAL_GIT_EXPORT_SNAPSHOT", "sblr.catalog.mutation.v3", "result.shape.external_git_snapshot_rows", "EngineExportExternalGitSnapshot", "authority.engine.catalog_artifact_api_required", true, true},
    {"AUDIT-0391B", "DIFF EXTERNAL GIT CATALOG SNAPSHOT", "artifact.external_git.diff_snapshot", "SBLR_ARTIFACT_EXTERNAL_GIT_DIFF_SNAPSHOT", "sblr.catalog.mutation.v3", "result.shape.external_git_diff_rows", "EngineDiffExternalGitSnapshot", "authority.engine.catalog_artifact_api_required", true, true},
    {"AUDIT-0391C", "PLAN EXTERNAL GIT CATALOG ROLLBACK", "artifact.external_git.rollback_plan", "SBLR_ARTIFACT_EXTERNAL_GIT_ROLLBACK_PLAN", "sblr.catalog.mutation.v3", "result.shape.external_git_rollback_plan_rows", "EnginePlanExternalGitRollback", "authority.engine.catalog_artifact_api_required", true, true},
    {"AUDIT-0392", "ENGINE IMPORT ROWS EXECUTE", "dml.execute_import_rows", "SBLR_DML_EXECUTE_IMPORT_ROWS", "sblr.dml.operation.v3", "result.shape.import_execution_status", "EngineExecuteImportRows", "authority.engine.dml_import_api_required", true, true},
    {"AUDIT-0393", "ENGINE IMPORT CHECKPOINT MODEL NORMALIZE", "dml.normalize_import_checkpoint_model", "SBLR_DML_IMPORT_CHECKPOINT_MODEL", "sblr.dml.operation.v3", "result.shape.import_checkpoint_model", "EngineNormalizeImportCheckpointModel", "authority.engine.dml_import_api_required", true, true},
    {"AUDIT-0394", "ENGINE IMPORT REJECT MODEL NORMALIZE", "dml.normalize_import_reject_model", "SBLR_DML_IMPORT_REJECT_MODEL", "sblr.dml.operation.v3", "result.shape.import_reject_model", "EngineNormalizeImportRejectModel", "authority.engine.dml_import_api_required", true, true},
    {"AUDIT-0395", "ENGINE DDL CREATE DATABASE", "ddl.create_database", "SBLR_DDL_CREATE_DATABASE", "sblr.catalog.mutation.v3", "result.shape.catalog_mutation_status", "EngineCreateDatabase", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0396", "ENGINE QUERY BIND EXPRESSION", "query.bind_expression", "SBLR_QUERY_BIND_EXPRESSION", "sblr.query.relational.v3", "result.shape.query_binding", "EngineBindExpression", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0397", "ENGINE QUERY BIND PREDICATE", "query.bind_predicate", "SBLR_QUERY_BIND_PREDICATE", "sblr.query.relational.v3", "result.shape.query_binding", "EngineBindPredicate", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0398", "ENGINE QUERY BIND PROJECTION", "query.bind_projection", "SBLR_QUERY_BIND_PROJECTION", "sblr.query.relational.v3", "result.shape.query_binding", "EngineBindProjection", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0399", "PREPARE TRANSACTION", "transaction.prepare", "SBLR_TRANSACTION_PREPARE", "sblr.transaction.control.v3", "result.shape.transaction_status", "EnginePrepareTransaction", "authority.engine.transaction_control_api_required", true, true},
    {"AUDIT-0400", "ENGINE CATALOG RESOLVE NAME", "catalog.resolve_name", "SBLR_CATALOG_RESOLVE_NAME", "sblr.catalog.mutation.v3", "result.shape.catalog_lookup", "EngineResolveName", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0401", "ENGINE CATALOG MAP UUID TO NAME", "catalog.map_uuid_to_name", "SBLR_CATALOG_MAP_UUID_TO_NAME", "sblr.catalog.mutation.v3", "result.shape.catalog_lookup", "EngineMapUuidToName", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0402", "ENGINE CATALOG LOOKUP OBJECT", "catalog.lookup_object", "SBLR_CATALOG_LOOKUP_OBJECT", "sblr.catalog.mutation.v3", "result.shape.catalog_lookup", "EngineLookupObject", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0403", "ENGINE CATALOG LIST CHILDREN", "catalog.list_children", "SBLR_CATALOG_LIST_CHILDREN", "sblr.catalog.mutation.v3", "result.shape.catalog_children", "EngineListCatalogChildren", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0404", "ENGINE CATALOG GET DEPENDENCIES", "catalog.get_dependencies", "SBLR_CATALOG_GET_DEPENDENCIES", "sblr.catalog.mutation.v3", "result.shape.catalog_dependencies", "EngineGetDependencies", "authority.engine.catalog_api_required", false, true},
    {"AUDIT-0405", "ENGINE SECURITY CREATE IDENTITY", "security.create_identity", "SBLR_SECURITY_CREATE_IDENTITY", "sblr.security.mutation.v3", "result.shape.security_status", "EngineCreateIdentity", "authority.engine.security_mutation_api_required", true, true},
    {"AUDIT-0406", "ENGINE SECURITY ALTER IDENTITY", "security.alter_identity", "SBLR_SECURITY_ALTER_IDENTITY", "sblr.security.mutation.v3", "result.shape.security_status", "EngineAlterIdentity", "authority.engine.security_mutation_api_required", true, true},
    {"AUDIT-0407", "ENGINE SECURITY GRANT RIGHT", "security.grant_right", "SBLR_SECURITY_GRANT_RIGHT", "sblr.security.mutation.v3", "result.shape.security_grant", "EngineGrantRight", "authority.engine.security_mutation_api_required", true, true},
    {"AUDIT-0408", "ENGINE SECURITY REVOKE RIGHT", "security.revoke_right", "SBLR_SECURITY_REVOKE_RIGHT", "sblr.security.mutation.v3", "result.shape.security_grant", "EngineRevokeRight", "authority.engine.security_mutation_api_required", true, true},
    {"AUDIT-0409", "ENGINE SECURITY EVALUATE VISIBILITY", "security.evaluate_visibility", "SBLR_SECURITY_EVALUATE_VISIBILITY", "sblr.policy.operation.v3", "result.shape.security_decision", "EngineEvaluateVisibility", "authority.engine.security_inspection_api_required", false, true},
    {"AUDIT-0410", "ENGINE SECURITY EVALUATE POLICY", "security.evaluate_policy", "SBLR_SECURITY_EVALUATE_POLICY", "sblr.policy.operation.v3", "result.shape.security_decision", "EngineEvaluatePolicy", "authority.engine.security_inspection_api_required", false, true},
    {"AUDIT-0411", "ENGINE SECURITY EVALUATE DEEP ENFORCEMENT", "security.evaluate_deep_enforcement", "SBLR_SECURITY_EVALUATE_DEEP_ENFORCEMENT", "sblr.policy.operation.v3", "result.shape.security_decision", "EngineEvaluateDeepSecurity", "authority.engine.security_inspection_api_required", false, true},
    {"AUDIT-0412", "ENGINE MANAGEMENT INSPECT CONFIG", "management.inspect_config", "SBLR_MANAGEMENT_INSPECT_CONFIG", "sblr.management.runtime_operation.v3", "result.shape.management_config", "EngineInspectConfig", "authority.engine.management_runtime_api_required", false, true},
    {"AUDIT-0413", "ENGINE MANAGEMENT SET CONFIG", "management.set_config", "SBLR_MANAGEMENT_SET_CONFIG", "sblr.management.runtime_operation.v3", "result.shape.management_config_status", "EngineSetConfig", "authority.engine.management_runtime_api_required", false, true},
    {"AUDIT-0414", "ENGINE MANAGEMENT RESET CONFIG", "management.reset_config", "SBLR_MANAGEMENT_RESET_CONFIG", "sblr.management.runtime_operation.v3", "result.shape.management_config_status", "EngineResetConfig", "authority.engine.management_runtime_api_required", false, true},
    {"AUDIT-0415", "ENGINE MANAGEMENT PREPARE SUPPORT BUNDLE", "management.prepare_support_bundle", "SBLR_MANAGEMENT_PREPARE_SUPPORT_BUNDLE", "sblr.management.runtime_operation.v3", "result.shape.support_bundle_manifest", "EnginePrepareSupportBundle", "authority.engine.management_runtime_api_required", false, true},
    {"AUDIT-0416", "ALTER DATABASE ENTER RESTRICTED OPEN", "lifecycle.enter_restricted_open", "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN", "sblr.management.runtime_operation.v3", "result.shape.lifecycle_status", "EngineEnterRestrictedOpenLifecycle", "authority.engine.lifecycle_api_required", false, false},
    {"AUDIT-0417", "ALTER DATABASE EXIT RESTRICTED OPEN", "lifecycle.exit_restricted_open", "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN", "sblr.management.runtime_operation.v3", "result.shape.lifecycle_status", "EngineExitRestrictedOpenLifecycle", "authority.engine.lifecycle_api_required", false, false},
    {"AUDIT-0425", "REGISTER PARSER PACKAGE", "extensibility.register_parser_package", "SBLR_EXTENSIBILITY_REGISTER_PARSER_PACKAGE", "sblr.udr.operation.v3", "result.shape.parser_package_status", "EngineRegisterParserPackage", "authority.engine.parser_package_api_required", true, true},
    {"AUDIT-0426", "ENGINE QUERY EXTRACT VALUE", "query.extract_value", "SBLR_QUERY_EXTRACT_VALUE", "sblr.query.relational.v3", "result.shape.typed_value", "EngineExtractValue", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0427", "ENGINE QUERY SET OPERATION", "query.set_operation", "SBLR_QUERY_SET_OPERATION", "sblr.query.relational.v3", "result.shape.typed_value", "EngineSetOperation", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0428", "ENGINE QUERY APPLY NUMERIC OPERATION", "query.apply_numeric_operation", "SBLR_QUERY_APPLY_NUMERIC_OPERATION", "sblr.query.relational.v3", "result.shape.typed_value", "EngineApplyNumericOperation", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0429", "ENGINE QUERY CANONICALIZE DOCUMENT VALUE", "query.canonicalize_document_value", "SBLR_QUERY_CANONICALIZE_DOCUMENT_VALUE", "sblr.query.document.v3", "result.shape.typed_value", "EngineCanonicalizeDocumentValue", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0430", "ENGINE QUERY EVALUATE ADVANCED DATATYPE FAMILY", "query.evaluate_advanced_datatype_family", "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY", "sblr.query.relational.v3", "result.shape.datatype_family_evaluation", "EngineEvaluateAdvancedDatatypeFamily", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0431", "ENGINE QUERY VALIDATE DOMAIN VALUE", "query.validate_domain_value", "SBLR_QUERY_VALIDATE_DOMAIN_VALUE", "sblr.query.relational.v3", "result.shape.typed_value", "EngineValidateDomainValue", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0432", "ENGINE QUERY INVOKE DOMAIN METHOD", "query.invoke_domain_method", "SBLR_QUERY_INVOKE_DOMAIN_METHOD", "sblr.query.relational.v3", "result.shape.typed_value", "EngineInvokeDomainMethod", "authority.engine.query_runtime_api_required", false, true},
    {"AUDIT-0433", "UNLISTEN ALL NOTIFICATIONS", "session.notification.unlisten_all", "SBLR_EVENT_CHANNEL_UNLISTEN_ALL", "sblr.event.channel.v3", "result.shape.event_subscription_status", "EngineUnlistenSessionNotifications", "authority.engine.event_notification_api_required", false, true},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string_view ExpectedServerAdmissionFamily(const B003Row& row) {
  if (StartsWith(row.operation_id, "agents.")) return "sblr.management.control.v3";
  if (row.operation_id == "management.inspect_config") return "sblr.management.report.v3";
  if (row.operation_id == "management.set_config" ||
      row.operation_id == "management.reset_config" ||
      row.operation_id == "management.prepare_support_bundle") {
    return "sblr.management.control.v3";
  }
  if (StartsWith(row.operation_id, "lifecycle.")) return "sblr.database.management.v3";
  if (row.operation_id == "dml.execute_import_rows" ||
      row.operation_id == "dml.normalize_import_checkpoint_model" ||
      row.operation_id == "dml.normalize_import_reject_model") {
    return "sblr.bulk.import.v3";
  }
  return row.family;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnosticCode(const MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string EvidenceMessage(const B003Row& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.audit_id);
  rendered += ' ';
  rendered += row.operation_id;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-00000000e701";
  session.connection_uuid = "019f0000-0000-7000-8000-00000000e702";
  session.database_uuid = "019f0000-0000-7000-8000-00000000e703";
  session.catalog_epoch = 101;
  session.security_policy_epoch = 103;
  session.descriptor_epoch = 107;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-00000000e704";
  config.bundle_contract_id = "sbp_sbsql@sbsql-sblr-final-cleanup-b003";
  config.build_id = "sbsql-sblr-final-cleanup-b003";
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
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineDescriptor Descriptor(std::string canonical_type_name,
                                 std::string kind = "scalar",
                                 std::string encoded = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = std::move(kind);
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor =
      encoded.empty() ? "type=" + descriptor.canonical_type_name : std::move(encoded);
  return descriptor;
}

api::EngineTypedValue TypedValue(std::string type, std::string value) {
  api::EngineTypedValue out;
  out.descriptor = Descriptor(std::move(type));
  out.encoded_value = std::move(value);
  out.is_null = false;
  return out;
}

api::EngineRowValue Row(std::string uuid,
                        std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(uuid);
  row.fields = std::move(fields);
  return row;
}

api::EngineRequestContext EngineContext(bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sblr-final-cleanup-b003";
  context.security_context_present = security_context_present;
  context.database_path = "/tmp/sbsql_sblr_final_cleanup_b003.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-00000000e801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-00000000e802";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-00000000e803";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-00000000e804";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-00000000e805";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-00000000e806";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-00000000e807";
  context.local_transaction_id = 44;
  context.snapshot_visible_through_local_transaction_id = 44;
  context.catalog_generation_id = 101;
  context.security_epoch = 103;
  context.resource_epoch = 107;
  context.trace_tags = {
      "security.bootstrap",
      "group:ROOT",
      "role:ROOT",
      "right:CONNECT",
      "right:SEC_IDENTITY_ADMIN",
      "right:SEC_MEMBERSHIP_ADMIN",
      "right:SEC_GRANT_ADMIN",
      "right:POLICY_ADMIN",
      "right:OBS_SECURITY_INSPECT",
      "right:MANAGEMENT_RUNTIME_CONTROL",
  };
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const B003Row& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.b003.") +
                                             std::string(row.audit_id));
  envelope.result_shape = row.result_shape;
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = row.requires_transaction_context;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void AddObject(api::EngineApiRequest* request,
               std::string uuid,
               std::string kind) {
  request->target_object.uuid.canonical = std::move(uuid);
  request->target_object.object_kind = std::move(kind);
}

void AddRelatedObject(api::EngineApiRequest* request,
                      std::string uuid,
                      std::string kind) {
  api::EngineObjectReference object;
  object.uuid.canonical = std::move(uuid);
  object.object_kind = std::move(kind);
  request->related_objects.push_back(std::move(object));
}

api::EngineApiRequest ApiRequestForRow(const B003Row& row) {
  api::EngineApiRequest request;
  request.option_envelopes.push_back(std::string("result_shape_contract:") +
                                     std::string(row.result_shape));
  request.option_envelopes.push_back(std::string("engine_api_function:") +
                                     std::string(row.engine_api_function));
  AddObject(&request, "019f0000-0000-7000-8000-00000000e901", "object");

  if (StartsWith(row.operation_id, "agents.")) {
    request.option_envelopes.insert(request.option_envelopes.end(),
                                    {"policy_authorized:true",
                                     "evidence_sink_available:true",
                                     "metrics_fresh:true",
                                     "safety_fence_result:passed",
                                     "requested_pages:4",
                                     "requested_bytes:4096",
                                     "page_family:data",
                                     "page_type:heap",
                                     "dry_run:true"});
    if (Contains(row.operation_id, "index_")) {
      AddRelatedObject(&request, "019f0000-0000-7000-8000-00000000e902", "index");
    } else {
      AddRelatedObject(&request, "019f0000-0000-7000-8000-00000000e903", "filespace");
    }
  } else if (StartsWith(row.operation_id, "artifact.")) {
    if (StartsWith(row.operation_id, "artifact.external_git.")) {
      request.option_envelopes.push_back("external_git_policy:enabled");
    }
    request.rows.push_back(Row("artifact-row-1",
                               {{"artifact_format", TypedValue("text", "sb.catalog.artifact.v1")},
                                {"object_uuid", TypedValue("uuid", "019f0000-0000-7000-8000-00000000e904")},
                                {"object_kind", TypedValue("text", "schema")},
                                {"default_name", TypedValue("text", "b003_schema")},
                                {"payload", TypedValue("text", "localized_name=en,default,b003_schema,b003_schema,default")}}));
  } else if (StartsWith(row.operation_id, "dml.")) {
    AddObject(&request, "019f0000-0000-7000-8000-00000000e905", "table");
    request.option_envelopes.insert(request.option_envelopes.end(),
                                    {"source_kind:native_sbsql_import",
                                     "format_family:csv",
                                     "reject_mode:fail_fast",
                                     "checkpoint_mode:disabled",
                                     "source_uuid:019f0000-0000-7000-8000-00000000e906",
                                     "source_fingerprint:b003-import",
                                     "source_position:0",
                                     "estimated_row_count:1"});
    request.rows.push_back(Row("import-row-1",
                               {{"id", TypedValue("int64", "1")},
                                {"payload", TypedValue("text", "b003")}}));
  } else if (row.operation_id == "ddl.create_database") {
    request.localized_names.push_back({"en", "primary", "", "b003_database", true});
    request.option_envelopes.push_back("name:b003_database");
  } else if (StartsWith(row.operation_id, "catalog.")) {
    AddObject(&request, "019f0000-0000-7000-8000-00000000e907", "schema");
    request.localized_names.push_back({"en", "primary", "b003_schema", "b003_schema", true});
    AddRelatedObject(&request, "019f0000-0000-7000-8000-00000000e908", "table");
  } else if (StartsWith(row.operation_id, "security.")) {
    request.option_envelopes.insert(request.option_envelopes.end(),
                                    {"identity_kind:user",
                                     "principal_name:b003_user",
                                     "create_home_schema:false",
                                     "right:CONNECT",
                                     "grantee_uuid:019f0000-0000-7000-8000-00000000e909",
                                     "target_object_uuid:019f0000-0000-7000-8000-00000000e90a",
                                     "target_object_kind:table",
                                     "visibility_right:READ",
                                     "policy_uuid:019f0000-0000-7000-8000-00000000e90b",
                                     "required_right:READ"});
    AddObject(&request, "019f0000-0000-7000-8000-00000000e90a", "table");
  } else if (StartsWith(row.operation_id, "management.")) {
    request.option_envelopes.insert(request.option_envelopes.end(),
                                    {"name:b003_config",
                                     "engine_authorized_support_export",
                                     "support_bundle_policy_installed:true",
                                     "retention_policy_ref:support.bundle.default_retention.v1",
                                     "redaction_profile_ref:server.support_bundle.default_redaction.v1"});
  } else if (StartsWith(row.operation_id, "query.")) {
    if (row.operation_id == "query.evaluate_advanced_datatype_family") {
      request.descriptors.push_back(Descriptor("vector", "scalar", "type=vector;dimension=3"));
      request.option_envelopes.insert(request.option_envelopes.end(),
                                      {"advanced_operation:validate",
                                       "advanced_index:none",
                                       "descriptor_profile:dimension=3;element_type=real32",
                                       "vector_dimension:3"});
    } else if (row.operation_id == "query.canonicalize_document_value") {
      request.rows.push_back(Row("query-row-1",
                                 {{"value", TypedValue("json", "{\"b003\":true}")}}));
      request.option_envelopes.push_back("document_reference_profile:sbsql");
    } else if (row.operation_id == "query.apply_numeric_operation") {
      request.descriptors.push_back(Descriptor("int64"));
      request.rows.push_back(Row("query-row-1",
                                 {{"left", TypedValue("int64", "2")},
                                  {"right", TypedValue("int64", "3")}}));
      request.option_envelopes.insert(request.option_envelopes.end(),
                                      {"numeric_operation:add",
                                       "rounding:half_even"});
    } else if (row.operation_id == "query.extract_value") {
      request.rows.push_back(Row("query-row-1",
                                 {{"value", TypedValue("timestamp", "2026-05-20T00:00:00Z")}}));
      request.option_envelopes.push_back("field:year");
    } else {
      request.descriptors.push_back(Descriptor("text"));
      request.rows.push_back(Row("query-row-1",
                                 {{"value", TypedValue("text", "b003")},
                                  {"other", TypedValue("text", "b003")}}));
      request.option_envelopes.insert(request.option_envelopes.end(),
                                      {"set_operation:membership",
                                       "method:identity"});
    }
  } else if (row.operation_id == "transaction.prepare") {
    request.option_envelopes.push_back("prepare_name:b003_transaction");
  } else if (StartsWith(row.operation_id, "lifecycle.")) {
    request.option_envelopes.push_back("lifecycle:restricted_open");
  } else if (row.operation_id == "extensibility.register_parser_package") {
    request.option_envelopes.push_back("name:b003_parser_package");
  } else if (row.operation_id == "session.notification.unlisten_all") {
    request.option_envelopes.push_back("scope:session");
  }
  return request;
}

void RequireLowering(const B003Row& row) {
  const auto artifacts = RunPipeline(row.sql);
  if (artifacts.envelope.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.envelope.messages) << '\n';
  }
  Require(artifacts.bound.bound, EvidenceMessage(row, "parser_bind_lower", "row did not bind"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "SBLR verifier rejected row"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == row.family,
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == row.family,
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.result_shape_key == row.result_shape,
          EvidenceMessage(row, "parser_bind_lower", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          EvidenceMessage(row, "parser_bind_lower", "engine API function mismatch"));
  Require(!HasValue(artifacts.envelope.required_authority_steps,
                    "authority.engine.cluster_provider_boundary_required"),
          EvidenceMessage(row, "parser_bind_lower", "cluster provider boundary leaked"));
  Require(!Contains(artifacts.envelope.payload, row.audit_id),
          EvidenceMessage(row, "parser_bind_lower", "audit id leaked into production payload"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(row, "parser_bind_lower", "source text key was embedded"));
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":"),
          EvidenceMessage(row, "parser_bind_lower", "SQL text key was embedded"));

  if (row.engine_api_command_route) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.parser.no_sql_text_execution"),
            EvidenceMessage(row, "parser_bind_lower", "no-SQL authority missing"));
    Require(HasValue(artifacts.envelope.required_authority_steps, row.authority_step),
            EvidenceMessage(row, "parser_bind_lower", "domain API authority missing"));
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.cluster.provider_dispatch_not_required"),
            EvidenceMessage(row, "parser_bind_lower", "cluster non-provider authority missing"));
    Require(Contains(artifacts.envelope.payload, "\"engine_api_command_route\":true"),
            EvidenceMessage(row, "parser_bind_lower", "engine command payload flag missing"));
    Require(Contains(artifacts.envelope.payload, "\"canonical_sblr_operation\":\"") &&
                Contains(artifacts.envelope.payload, row.operation_id),
            EvidenceMessage(row, "parser_bind_lower", "canonical operation missing"));
    Require(Contains(artifacts.envelope.payload, "\"engine_api_function\":\"") &&
                Contains(artifacts.envelope.payload, row.engine_api_function),
            EvidenceMessage(row, "parser_bind_lower", "domain API function missing"));
    Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false"),
            EvidenceMessage(row, "parser_bind_lower", "cluster provider dispatch evidence missing"));
    Require(Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
            EvidenceMessage(row, "parser_bind_lower", "private cluster evidence missing"));
    Require(Contains(artifacts.envelope.payload,
                     row.requires_transaction_context
                         ? "\"mga_transaction_context_required\":true"
                         : "\"mga_transaction_context_required\":false"),
            EvidenceMessage(row, "parser_bind_lower", "transaction context evidence mismatch"));
  } else {
    Require(Contains(artifacts.envelope.payload, "\"scratchbird_lifecycle_api\":true"),
            EvidenceMessage(row, "parser_bind_lower", "canonical lifecycle API flag missing"));
    Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
            EvidenceMessage(row, "parser_bind_lower", "lifecycle no-SQL evidence missing"));
  }

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected row"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == ExpectedServerAdmissionFamily(row),
          EvidenceMessage(row, "server_admission", "operation family mismatch"));
}

void RequireDispatch(const B003Row& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, EvidenceMessage(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, EvidenceMessage(row, "sblr_registry", "opcode mismatch"));
  const auto envelope = EngineEnvelope(row);
  const auto registry_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(registry_validation.ok,
          EvidenceMessage(row, "sblr_registry", "registry validation rejected row"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, ApiRequestForRow(row)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Require(!Contains(diagnostic.code, "CLUSTER") &&
                !Contains(diagnostic.message_key, "cluster"),
            EvidenceMessage(row, "engine_dispatch", "cluster diagnostic leaked"));
    Require(!Contains(diagnostic.code, "UNKNOWN_OPERATION") &&
                !Contains(diagnostic.message_key, "unknown_operation"),
            EvidenceMessage(row, "engine_dispatch", "unknown dispatch diagnostic leaked"));
    Require(!Contains(diagnostic.detail, "not_implemented") &&
                !Contains(diagnostic.detail, "TODO"),
            EvidenceMessage(row, "engine_dispatch", "placeholder diagnostic leaked"));
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine envelope was not valid"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not accept row"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not route to API"));
  Require(result.api_result.operation_id == row.operation_id,
          EvidenceMessage(row, "engine_dispatch", "API operation id mismatch"));
  Require(!result.api_result.cluster_authority_required,
          EvidenceMessage(row, "engine_dispatch", "cluster authority was required"));
}

void RequireInvalidSyntaxDiagnostics() {
  const auto artifacts = RunPipeline("ENGINE QUERY APPLY NUMERIC");
  Require(artifacts.envelope.messages.has_errors(),
          "invalid engine API command shape did not produce a message-vector error");
  Require(HasDiagnosticCode(artifacts.envelope.messages,
                            "SBSQL.ENGINE_API_COMMAND.INVALID_SHAPE"),
          "invalid engine API command shape did not produce the expected diagnostic code");
  const std::string rendered = RenderMessageVectorSet(artifacts.envelope.messages);
  Require(Contains(rendered, "SBSQL.ENGINE_API_COMMAND.INVALID_SHAPE"),
          "rendered engine API command message vector omitted diagnostic code");
  Require(Contains(rendered, "engine_api_command"),
          "rendered engine API command message vector omitted surface field");
}

void RequireSecurityRefusalRedaction() {
  const B003Row* security_row = nullptr;
  for (const auto& row : kRows) {
    if (row.operation_id == "security.grant_right") {
      security_row = &row;
      break;
    }
  }
  Require(security_row != nullptr, "security refusal row missing from B003 route table");
  const auto& row = *security_row;
  auto request = ApiRequestForRow(row);
  request.option_envelopes.push_back("target_object_uuid:secret_customer_table");
  auto context = EngineContext();
  context.trace_tags = {"right:CONNECT", "deny:SEC_GRANT_ADMIN"};
  const sblr::SblrDispatchRequest dispatch{context,
                                           EngineEnvelope(row),
                                           request};
  const auto result = sblr::DispatchSblrOperation(dispatch);
  Require(result.dispatched_to_api, "security refusal did not reach engine API");
  Require(!result.api_result.ok, "security refusal unexpectedly succeeded");
  Require(!result.api_result.diagnostics.empty(), "security refusal omitted diagnostics");
  for (const auto& diagnostic : result.api_result.diagnostics) {
    Require(diagnostic.detail.find("secret_customer_table") == std::string::npos,
            "security refusal leaked target reference in diagnostic detail");
  }
}

void RequireOverlapRoutesPreserved() {
  const auto plural_jobs = RunPipeline("SHOW JOBS");
  Require(plural_jobs.verifier.admitted, "SHOW JOBS verifier rejected plural route");
  Require(plural_jobs.envelope.operation_id == "observability.show_jobs",
          "SHOW JOBS route was hijacked by engine API command routes");
  Require(!Contains(plural_jobs.envelope.payload, "\"engine_api_command_route\":true"),
          "SHOW JOBS was marked as an engine API command route");

  const auto udr_packages = RunPipeline("SHOW UDR PACKAGES");
  Require(udr_packages.verifier.admitted, "SHOW UDR PACKAGES verifier rejected existing route");
  Require(udr_packages.envelope.operation_id == "extensibility.inspect_udr_packages",
          "SHOW UDR PACKAGES route was hijacked by parser package route");

  const auto prepared_statement = RunPipeline("PREPARE stmt AS SELECT 1 AS value");
  Require(prepared_statement.verifier.admitted, "PREPARE statement verifier rejected existing route");
  Require(prepared_statement.envelope.operation_id == "session.prepare_statement",
          "prepared statement route was hijacked by PREPARE TRANSACTION route");

  const auto unlisten_channel = RunPipeline("UNLISTEN EVENT CHANNEL updates");
  Require(unlisten_channel.envelope.operation_id != "session.notification.unlisten_all",
          "UNLISTEN EVENT CHANNEL route was hijacked by UNLISTEN ALL NOTIFICATIONS");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 26> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "B002Exact",
      "IsB002",
      "b002_",
      "_b002",
      "B003Exact",
      "IsB003",
      "b003_",
      "_b003",
      "EngineRunSbsqlSblrFinalCleanup",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
      "SSFC-B003",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kRows) {
    RequireLowering(row);
    RequireDispatch(row);
  }
  RequireInvalidSyntaxDiagnostics();
  RequireSecurityRefusalRedaction();
  RequireOverlapRoutesPreserved();
  std::cout << "sbsql_sblr_final_cleanup_b003_engine_api_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
