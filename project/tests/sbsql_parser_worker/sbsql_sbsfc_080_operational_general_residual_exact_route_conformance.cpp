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
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000080001";

struct CaseRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sql;
  std::string_view operation_family;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view engine_api_function;
  std::string_view runtime_evidence_kind;
  std::string_view runtime_evidence_id;
  std::string_view target_object_kind;
  bool transaction_authority;
  bool cluster_profile_metadata;
};

const CaseRow kCases[] = {
    {"SBSQL-00050CEB8798", "validator_clause", "grammar_production", "VALIDATOR CLAUSE checksum;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "validator_clause", "operational_control_descriptor", false, false},
    {"SBSQL-0061DA1A97F9", "refusal_stmt", "grammar_production", "REFUSAL STMT policy;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "refusal_stmt", "operational_control_descriptor", false, false},
    {"SBSQL-0DDE985BA8C7", "subject_capability", "grammar_production", "SUBJECT CAPABILITY read;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "subject_capability", "operational_control_descriptor", false, false},
    {"SBSQL-135816922546", "edition_refusal_stmt", "grammar_production", "EDITION REFUSAL STMT;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "edition_refusal_stmt", "operational_control_descriptor", false, false},
    {"SBSQL-1978468B30C7", "config_stmt", "grammar_production", "CONFIG STMT inspect;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "config_stmt", "management_descriptor", false, false},
    {"SBSQL-19E34D75C5DA", "extension_action", "grammar_production", "EXTENSION ACTION load;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "extension_action", "management_descriptor", false, false},
    {"SBSQL-2287012D5F69", "quota_subject", "grammar_production", "QUOTA SUBJECT tenant;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "quota_subject", "operational_control_descriptor", false, false},
    {"SBSQL-36CFF650413E", "settings_clause", "grammar_production", "SETTINGS CLAUSE local;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "settings_clause", "management_descriptor", false, false},
    {"SBSQL-3BA7AB82468E", "verification_clause", "grammar_production", "VERIFICATION CLAUSE checksum;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "verification_clause", "operational_control_descriptor", false, false},
    {"SBSQL-3BB66DAAEF61", "pragma_target", "grammar_production", "PRAGMA TARGET session;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "pragma_target", "management_descriptor", false, false},
    {"SBSQL-455B43B189CA", "audit_class", "grammar_production", "AUDIT CLASS security;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "audit_class", "operational_control_descriptor", false, false},
    {"SBSQL-4A225D698CD4", "extension_stmt", "grammar_production", "EXTENSION STMT inspect;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "extension_stmt", "management_descriptor", false, false},
    {"SBSQL-4C8F3587FE38", "session_setting_target", "grammar_production", "SESSION SETTING TARGET search_path;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "session_setting_target", "session_descriptor", false, false},
    {"SBSQL-4ECCE8E30CE0", "extension_attr", "grammar_production", "EXTENSION ATTR trusted;", "sblr.management.runtime_operation.v3", "management.inspect_runtime", "SBLR_MANAGEMENT_INSPECT_RUNTIME", "EngineInspectManagementRuntime", "management_descriptor_route", "extension_attr", "management_descriptor", false, false},
    {"SBSQL-58B069CCA20C", "session_setting_stmt", "grammar_production", "SESSION SETTING STMT set;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "session_setting_stmt", "session_descriptor", false, false},
    {"SBSQL-59F5EA4F0353", "subject_area_op_stmt", "grammar_production", "SUBJECT AREA OP inspect;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "subject_area_op_stmt", "operational_control_descriptor", false, false},
    {"SBSQL-62644DADEB87", "audit_target", "grammar_production", "AUDIT TARGET table;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "audit_target", "operational_control_descriptor", false, false},
    {"SBSQL-64C6C4A1357F", "event_trigger_audit_clause", "grammar_production", "EVENT TRIGGER AUDIT clause;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "event_trigger_audit_clause", "operational_control_descriptor", false, false},
    {"SBSQL-6C2994D68FE4", "masking_options", "grammar_production", "MASKING OPTIONS redacted;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "masking_options", "operational_control_descriptor", false, false},
    {"SBSQL-71D1C5165313", "disconnect_session", "canonical_surface", "DISCONNECT SESSION 1;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "disconnect_session", "session_descriptor", false, false},
    {"SBSQL-7D0503AE3510", "refusal_diagnostic", "grammar_production", "REFUSAL DIAGNOSTIC code;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "refusal_diagnostic", "operational_control_descriptor", false, false},
    {"SBSQL-9AD5007A5B32", "subject_area_name", "grammar_production", "SUBJECT AREA NAME sys;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "subject_area_name", "operational_control_descriptor", false, false},
    {"SBSQL-AD3FF08DFA03", "refusal_target", "grammar_production", "REFUSAL TARGET statement;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "refusal_target", "operational_control_descriptor", false, false},
    {"SBSQL-B8F962CC8DBD", "quota_key", "grammar_production", "QUOTA KEY storage;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "quota_key", "operational_control_descriptor", false, false},
    {"SBSQL-C43243227BEC", "session_id", "grammar_production", "SESSION ID current;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "session_id", "session_descriptor", false, false},
    {"SBSQL-DC0192B217F7", "connect_session", "canonical_surface", "CONNECT SESSION user;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "connect_session", "session_descriptor", false, false},
    {"SBSQL-F6C4E9705A12", "set_session", "canonical_surface", "SET SESSION work_mem;", "sblr.observability.inspect.v3", "observability.show_sessions", "SBLR_OBSERVABILITY_SHOW_SESSIONS", "EngineShowSessions", "session_control_route", "set_session", "session_descriptor", false, false},
    {"SBSQL-F8C5919F9523", "event_audit_clause", "grammar_production", "EVENT AUDIT CLAUSE;", "sblr.observability.inspect.v3", "observability.show_diagnostics", "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", "EngineShowDiagnostics", "operational_observability_route", "event_audit_clause", "operational_control_descriptor", false, false},
    {"SBSQL-09A8779C83D6", "set_constraints_stmt", "grammar_production", "SET CONSTRAINTS ALL DEFERRED;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "set_constraints_stmt", "transaction_descriptor", true, false},
    {"SBSQL-3D2A1A09C4FB", "snapshot_stmt", "grammar_production", "SNAPSHOT STMT export;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "snapshot_stmt", "transaction_descriptor", true, false},
    {"SBSQL-45CFA1C24D6A", "snapshot_name", "grammar_production", "SNAPSHOT NAME snap_a;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "snapshot_name", "transaction_descriptor", true, false},
    {"SBSQL-46DC7F923118", "atomicity_modifier", "grammar_production", "ATOMICITY MODIFIER atomic;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "atomicity_modifier", "transaction_descriptor", true, false},
    {"SBSQL-64AC5F07A4E4", "iso_level", "grammar_production", "ISO LEVEL snapshot;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "iso_level", "transaction_descriptor", true, false},
    {"SBSQL-8DA8EAC53B75", "named_snapshot_stmt", "grammar_production", "NAMED SNAPSHOT snap_a;", "sblr.transaction.control.v3", "transaction.set_characteristics", "SBLR_TRANSACTION_SET_CHARACTERISTICS", "EngineSetTransactionCharacteristics", "mga_transaction_authority_route", "named_snapshot_stmt", "transaction_descriptor", true, false},
    {"SBSQL-00D0771155C2", "colocation_clause", "grammar_production", "COLOCATION CLAUSE group_a;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "colocation_clause", "cluster_profile_descriptor", false, true},
    {"SBSQL-02CA72FF1C71", "locality_clause", "grammar_production", "LOCALITY CLAUSE local;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "locality_clause", "cluster_profile_descriptor", false, true},
    {"SBSQL-19CB2172CF3D", "zone_target", "grammar_production", "ZONE TARGET region;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "zone_target", "cluster_profile_descriptor", false, true},
    {"SBSQL-3FB8BACBF197", "locality_spec", "grammar_production", "LOCALITY SPEC rack;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "locality_spec", "cluster_profile_descriptor", false, true},
    {"SBSQL-D85CE20AD873", "zone_setting_assign", "grammar_production", "ZONE SETTING ASSIGN replicas;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "zone_setting_assign", "cluster_profile_descriptor", false, true},
    {"SBSQL-FDB2AC4910D6", "ignite_zone_clause", "grammar_production", "IGNITE ZONE CLAUSE;", "sblr.cluster.private_operation.v3", "cluster.profile_operation", "SBLR_CLUSTER_PROFILE_OPERATION", "EngineClusterProfileOperation", "cluster_profile_route", "ignite_zone_clause", "cluster_profile_descriptor", false, true},
    {"SBSQL-044636D5F226", "pipeline_clause", "grammar_production", "PIPELINE CLAUSE staged;", "sblr.observability.inspect.v3", "observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "EngineShowAcceleration", "acceleration_profile_route", "pipeline_clause", "acceleration_descriptor", false, false},
    {"SBSQL-05DB282498F4", "acceleration_stmt", "grammar_production", "ACCELERATION STMT inspect;", "sblr.observability.inspect.v3", "observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "EngineShowAcceleration", "acceleration_profile_route", "acceleration_stmt", "acceleration_descriptor", false, false},
    {"SBSQL-1DF44A9DC689", "buffer_action", "grammar_production", "BUFFER ACTION sweep;", "sblr.storage.management_operation.v3", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "EngineStorageManagementOperation", "storage_management_operation", "buffer_action", "storage_management_descriptor", false, false},
    {"SBSQL-1FF7927EEEC7", "kernel_name", "grammar_production", "KERNEL NAME default;", "sblr.observability.inspect.v3", "observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "EngineShowAcceleration", "acceleration_profile_route", "kernel_name", "acceleration_descriptor", false, false},
    {"SBSQL-48A533677977", "sweep_control_stmt", "grammar_production", "SWEEP CONTROL STMT;", "sblr.storage.management_operation.v3", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "EngineStorageManagementOperation", "storage_management_operation", "sweep_control_stmt", "storage_management_descriptor", false, false},
    {"SBSQL-498A9CC3FA73", "engine_name", "grammar_production", "ENGINE NAME scratchbird;", "sblr.storage.management_operation.v3", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "EngineStorageManagementOperation", "storage_management_operation", "engine_name", "storage_management_descriptor", false, false},
    {"SBSQL-4BA150E33932", "compile_options", "grammar_production", "COMPILE OPTIONS safe;", "sblr.observability.inspect.v3", "observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "EngineShowAcceleration", "acceleration_profile_route", "compile_options", "acceleration_descriptor", false, false},
    {"SBSQL-5D5A154DE8A9", "optimization_level", "grammar_production", "OPTIMIZATION LEVEL baseline;", "sblr.observability.inspect.v3", "observability.show_acceleration", "SBLR_OBSERVABILITY_SHOW_ACCELERATION", "EngineShowAcceleration", "acceleration_profile_route", "optimization_level", "acceleration_descriptor", false, false},
    {"SBSQL-6DD47DBC3238", "compression_spec", "grammar_production", "COMPRESSION SPEC none;", "sblr.storage.management_operation.v3", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "EngineStorageManagementOperation", "storage_management_operation", "compression_spec", "storage_management_descriptor", false, false},
    {"SBSQL-72F539F656EE", "engine_clause", "grammar_production", "ENGINE CLAUSE local;", "sblr.storage.management_operation.v3", "storage.manage_operation", "SBLR_STORAGE_MANAGEMENT_OPERATION", "EngineStorageManagementOperation", "storage_management_operation", "engine_clause", "storage_management_descriptor", false, false},
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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000080101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000080102";
  session.database_uuid = "019f0000-0000-7000-8000-000000080103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 80;
  session.security_policy_epoch = 81;
  session.descriptor_epoch = 82;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_080_operational_general_residual";
  config.parser_uuid = "019f0000-0000-7000-8000-000000080104";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-080-operational-general-residual";
  config.build_id = "sbsql-sbsfc-080-operational-general-residual";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(const CaseRow& row) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(row.sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const CaseRow& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr, "SBSFC-080 generated registry row missing");
  Require(registry_row->canonical_name == row.canonical_name,
          "SBSFC-080 generated registry canonical name drifted");
  Require(registry_row->surface_kind == row.surface_kind,
          "SBSFC-080 generated registry surface kind drifted");
  Require(registry_row->source_status == "native_now",
          "SBSFC-080 generated registry status drifted");
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          "SBSFC-080 generated registry cluster scope drifted");
  Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
          "SBSFC-080 generated registry canonical family drifted");
}

void RequireExactLowering(const CaseRow& row, const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-080 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-080 AST failed");
  Require(artifacts.ast.statement_surface_id == row.surface_id,
          std::string("SBSFC-080 AST row surface id mismatch: expected=") +
              std::string(row.surface_id) + " actual=" + artifacts.ast.statement_surface_id +
              " sql=" + std::string(row.sql));
  Require(artifacts.ast.statement_surface_name == row.canonical_name,
          std::string("SBSFC-080 AST canonical name mismatch: expected=") +
              std::string(row.canonical_name) +
              " actual=" + artifacts.ast.statement_surface_name +
              " sql=" + std::string(row.sql));
  Require(artifacts.ast.registry_family == "sbsql.general.operation.v3",
          std::string("SBSFC-080 AST registry family mismatch: actual=") +
              artifacts.ast.registry_family + " sql=" + std::string(row.sql));
  Require(artifacts.ast.operation_family == "sblr.general.operation.v3",
          "SBSFC-080 AST canonical operation family mismatch");
  Require(artifacts.bound.bound, "SBSFC-080 bind failed");
  Require(artifacts.verifier.admitted, "SBSFC-080 verifier rejected exact route");
  Require(artifacts.envelope.operation_family == row.operation_family,
          "SBSFC-080 route operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == row.operation_family,
          "SBSFC-080 route operation key mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "SBSFC-080 operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "SBSFC-080 engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode, "SBSFC-080 opcode mismatch");
  Require(artifacts.envelope.engine_api_function == row.engine_api_function,
          "SBSFC-080 engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-080 parser no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-080 parser no-finality authority missing");
  if (row.transaction_authority) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.mga_transaction_control_required"),
            "SBSFC-080 MGA transaction authority missing");
  }
  if (row.cluster_profile_metadata) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.cluster.provider_dispatch_not_required"),
            "SBSFC-080 cluster provider exclusion authority missing");
  }
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "SBSFC-080 payload missing row surface id");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_kind),
          "SBSFC-080 payload missing runtime evidence kind");
  Require(Contains(artifacts.envelope.payload, row.runtime_evidence_id),
          "SBSFC-080 payload missing runtime evidence id");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-080 lowering allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, row.sql),
          "SBSFC-080 payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SBSQL_SURFACE_REPLAY") &&
              !Contains(artifacts.envelope.payload, "exact_refusal") &&
              !Contains(artifacts.envelope.payload, "cluster_support_not_enabled"),
          "SBSFC-080 payload used replay, refusal, or cluster-provider error evidence");
  Require(Contains(artifacts.envelope.payload, "\"cluster_provider_dispatch\":false") &&
              Contains(artifacts.envelope.payload, "\"private_cluster_execution\":false"),
          "SBSFC-080 payload did not prove no cluster/private dispatch");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal_recovery_authority\":true") &&
              !Contains(artifacts.envelope.payload, "recovery_authority\":true"),
          "SBSFC-080 payload carried WAL/recovery authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  if (row.cluster_profile_metadata) {
    Require(!admission.admitted,
            "SBSFC-080 cluster profile metadata route did not fail closed at server admission");
    Require(!admission.diagnostics.empty() &&
                admission.diagnostics.front().code == "SBLR.FAMILY_RECONCILIATION_REQUIRED",
            "SBSFC-080 cluster profile metadata route did not emit family reconciliation diagnostic");
  } else {
    Require(admission.admitted, "SBSFC-080 server admission rejected exact route");
    Require(admission.requires_public_abi_dispatch,
            "SBSFC-080 server admission did not require public ABI dispatch");
    Require(admission.operation_id == row.operation_id,
            "SBSFC-080 server admission operation id mismatch");
    Require(admission.operation_family == row.operation_family,
            "SBSFC-080 server admission operation family mismatch");
  }

  const auto* opcode = sblr::LookupSblrOperation(std::string(row.operation_id));
  Require(opcode != nullptr, "SBSFC-080 opcode registry row missing");
  Require(opcode->opcode == row.opcode, "SBSFC-080 opcode registry drifted");
  Require(opcode->requires_cluster_authority == false,
          "SBSFC-080 opcode claimed cluster authority");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_sbsfc_080_operational_general_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810800000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810800001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810800002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "SBSFC-080 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sbsfc-080-operational-general-residual";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000080201";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000080202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000080203";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000080204";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000080205";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.current_sqlstate = "00000";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-000000080206";
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

sblr::SblrOperationEnvelope EngineEnvelope(const CaseRow& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.sbsfc080.operational_general");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands.push_back({"text", "target_object_uuid", std::string(kTargetUuid)});
  envelope.operands.push_back({"text", "target_object_kind", std::string(row.target_object_kind)});
  envelope.operands.push_back({"text", "sbsfc080_surface_id", std::string(row.surface_id)});
  envelope.operands.push_back({"text", "sbsfc080_runtime_evidence_kind", std::string(row.runtime_evidence_kind)});
  envelope.operands.push_back({"text", "sbsfc080_runtime_evidence_id", std::string(row.runtime_evidence_id)});
  envelope.operands.push_back({"text", "runtime_component", "parsers"});
  envelope.operands.push_back({"text", "transaction_read_mode", "read_write"});
  envelope.operands.push_back({"text", "transaction_isolation_level", "snapshot"});
  envelope.operands.push_back({"text", "storage_action", std::string(row.runtime_evidence_id)});
  envelope.operands.push_back({"text", "cluster_profile_action", "profile_metadata"});
  return envelope;
}

api::EngineApiRequest ApiRequestFor(const CaseRow& row) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = std::string(row.target_object_kind);
  request.option_envelopes.push_back(std::string("sbsfc080_surface_id:") + std::string(row.surface_id));
  request.option_envelopes.push_back(std::string("sbsfc080_runtime_evidence_kind:") + std::string(row.runtime_evidence_kind));
  request.option_envelopes.push_back(std::string("sbsfc080_runtime_evidence_id:") + std::string(row.runtime_evidence_id));
  request.option_envelopes.push_back("runtime_component:parsers");
  request.option_envelopes.push_back("transaction_read_mode:read_write");
  request.option_envelopes.push_back("transaction_isolation_level:snapshot");
  request.option_envelopes.push_back(std::string("storage_action:") + std::string(row.runtime_evidence_id));
  request.option_envelopes.push_back("cluster_profile_action:profile_metadata");
  return request;
}

void RequireEngineDispatch(const api::EngineRequestContext& context, const CaseRow& row) {
  const auto result = sblr::DispatchSblrOperation({context, EngineEnvelope(row), ApiRequestFor(row)});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "SBSFC-080 engine envelope rejected");
  Require(result.accepted, "SBSFC-080 engine dispatch did not accept route");
  Require(result.dispatched_to_api, "SBSFC-080 engine did not dispatch to API");
  Require(result.api_result.operation_id == row.operation_id,
          "SBSFC-080 runtime operation id drifted");
  Require(result.api_result.ok, "SBSFC-080 runtime API did not complete");
  Require(HasEvidence(result.api_result, row.runtime_evidence_kind, row.runtime_evidence_id),
          "SBSFC-080 runtime evidence missing");
  Require(HasEvidence(result.api_result, "sbsfc080_surface", row.surface_id),
          "SBSFC-080 runtime did not carry row surface evidence");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "SBSFC-080 runtime allowed parser SQL execution");
  Require(HasEvidence(result.api_result, "cluster_provider_dispatch", "false"),
          "SBSFC-080 runtime claimed cluster provider dispatch");
  Require(HasEvidence(result.api_result, "private_cluster_execution", "false"),
          "SBSFC-080 runtime claimed private cluster execution");
  Require(HasEvidence(result.api_result, "wal_recovery_authority", "false"),
          "SBSFC-080 runtime carried WAL/recovery authority");
  if (row.transaction_authority) {
    Require(HasEvidence(result.api_result, "mga_authority", "session_default_only_no_finality") ||
                HasEvidence(result.api_result, "mga_authority", "durable_transaction_inventory"),
            "SBSFC-080 transaction row did not carry MGA authority evidence");
    Require(HasEvidence(result.api_result, "parser_finality", "false"),
            "SBSFC-080 transaction row allowed parser finality");
  }
}

}  // namespace

int main() {
  static_assert(sizeof(kCases) / sizeof(kCases[0]) == 50);
  for (const auto& row : kCases) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row, RunPipeline(row));
  }

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = EngineContext(path, database_uuid);
  for (const auto& row : kCases) {
    RequireEngineDispatch(context, row);
  }
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_sbsfc_080_operational_general_residual_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
