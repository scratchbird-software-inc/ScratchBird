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
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000002301";
constexpr std::string_view kGranteeUuid = "019f0000-0000-7000-8000-000000002302";
constexpr std::string_view kPolicyUuid = "019f0000-0000-7000-8000-000000002303";
constexpr std::string_view kPolicyTargetUuid = "019f0000-0000-7000-8000-000000002304";
constexpr std::string_view kRoleUuid = "019f0000-0000-7000-8000-000000002305";
constexpr std::string_view kUserUuid = "019f0000-0000-7000-8000-000000002306";
constexpr std::string_view kEventTriggerUuid = "019f0000-0000-7000-8000-000000002307";

struct SecurityRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view validation_fixture_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view registry_family = "security";
  std::string_view source_sblr_operation_family;
  std::string_view parser_handler_key = "parser.statement_family.security";
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
};

constexpr std::array<SecurityRowEvidence, 29> kSecurityRows{{
    {"SBSQL-B8873CC0BD58",
     "grant",
     "canonical_surface",
     "SBSQL-SURFACE-E6448F65FFEF",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-F86AC3DCC60A",
     "grant_stmt",
     "grammar_production",
     "SBSQL-SURFACE-6DE1852FCFAF",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-BA4E2B671F4B",
     "grant_target",
     "grammar_production",
     "SBSQL-SURFACE-65F341E8F398",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-DE069CC59237",
     "grantee",
     "grammar_production",
     "SBSQL-SURFACE-990E7DD61EB9",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-0871BF001B4D",
     "grantee_list",
     "grammar_production",
     "SBSQL-SURFACE-5331D6609BBC",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-8E8A494388C0",
     "dcl_security_stmt",
     "grammar_production",
     "SBSQL-SURFACE-4194E4B2EA3D",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT"},
    {"SBSQL-B2958D85DBE3",
     "revoke",
     "canonical_surface",
     "SBSQL-SURFACE-1D815626F2AC",
     "REVOKE SELECT ON customer FROM app_role",
     "security.privilege.revoke",
     "SBLR_SECURITY_PRIVILEGE_REVOKE"},
    {"SBSQL-4FAA221A7195",
     "revoke_stmt",
     "grammar_production",
     "SBSQL-SURFACE-DB1ADA41BB75",
     "REVOKE SELECT ON customer FROM app_role",
     "security.privilege.revoke",
     "SBLR_SECURITY_PRIVILEGE_REVOKE"},
    {"SBSQL-1764F6126D65",
     "attach_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-EC7A740F924E",
     "ATTACH POLICY placement_policy TO FILESPACE primary ROLE app_role",
     "security.policy.attach",
     "SBLR_SECURITY_POLICY_ATTACH"},
    {"SBSQL-BFDBC35B30F1",
     "placement_policy_name",
     "grammar_production",
     "SBSQL-SURFACE-785A1A525E66",
     "ATTACH POLICY placement_policy TO FILESPACE primary ROLE app_role",
     "security.policy.attach",
     "SBLR_SECURITY_POLICY_ATTACH"},
    {"SBSQL-9292E516EC56",
     "filespace_role",
     "grammar_production",
     "SBSQL-SURFACE-3486C2F62892",
     "ATTACH POLICY placement_policy TO FILESPACE primary ROLE app_role",
     "security.policy.attach",
     "SBLR_SECURITY_POLICY_ATTACH"},
    {"SBSQL-3C1DF693914F",
     "user_name",
     "grammar_production",
     "SBSQL-SURFACE-2C3908E0237F",
     "ATTACH POLICY user_policy TO USER app_user",
     "security.policy.attach",
     "SBLR_SECURITY_POLICY_ATTACH"},
    {"SBSQL-E8CCAF7D09B8",
     "event_trigger_security_clause",
     "grammar_production",
     "SBSQL-SURFACE-1DB2E874A555",
     "ATTACH POLICY event_policy TO EVENT TRIGGER audit_trigger SECURITY DEFINER",
     "security.policy.attach",
     "SBLR_SECURITY_POLICY_ATTACH"},
    {"SBSQL-5BC7985C2B11",
     "activate_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-786868226156",
     "ACTIVATE POLICY app_policy",
     "security.policy.activate",
     "SBLR_SECURITY_POLICY_ACTIVATE"},
    {"SBSQL-7872F03C1FC7",
     "deactivate_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-55704D573ED3",
     "DEACTIVATE POLICY app_policy",
     "security.policy.deactivate",
     "SBLR_SECURITY_POLICY_DEACTIVATE"},
    {"SBSQL-811AB9F5B009",
     "validate_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-FC099387BE74",
     "VALIDATE POLICY app_policy",
     "security.policy.validate",
     "SBLR_SECURITY_POLICY_VALIDATE"},
    {"SBSQL-CCE2E0A8B006",
     "show_security_policy",
     "grammar_production",
     "SBSQL-SURFACE-EABEDB2E3014",
     "SHOW SECURITY POLICY app_policy",
     "security.policy.show",
     "SBLR_SECURITY_POLICY_SHOW"},
    {"SBSQL-360A316CB38A",
     "set_role_stmt",
     "grammar_production",
     "SBSQL-SURFACE-DF68C0F49E93",
     "SET ROLE app_role",
     "security.session.set_role",
     "SBLR_SECURITY_SESSION_SET_ROLE"},
    {"SBSQL-D01B5B47DD70",
     "role_name",
     "grammar_production",
     "SBSQL-SURFACE-ED385E9FF4BB",
     "SET ROLE app_role",
     "security.session.set_role",
     "SBLR_SECURITY_SESSION_SET_ROLE"},
    {"SBSQL-6C5AF5C25DA2",
     "policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-CFACA20E9736",
     "ACTIVATE POLICY app_policy",
     "security.policy.activate",
     "SBLR_SECURITY_POLICY_ACTIVATE"},
    {"SBSQL-539F0D133459",
     "policy_name",
     "grammar_production",
     "SBSQL-SURFACE-7AC1EFD9EDBE",
     "ACTIVATE POLICY app_policy",
     "security.policy.activate",
     "SBLR_SECURITY_POLICY_ACTIVATE"},
    {"SBSQL-A33FD38A6F9D",
     "create_principal_stmt",
     "grammar_production",
     "SBSQL-SURFACE-6C54A420B42B",
     "CREATE PRINCIPAL app_user TYPE USER CREDENTIAL REF protected_ref_001",
     "security.principal.create",
     "SBLR_SECURITY_PRINCIPAL_CREATE",
     "security",
     "sblr.security.mutation.v3",
     "parser.statement_family.security",
     "lowering.sblr_family.sblr_security_mutation_v3",
     "server.admission.sblr_security_mutation_v3",
     "engine.rule.sblr_security_mutation_v3"},
    {"SBSQL-99E0610C0C88",
     "alter_principal_stmt",
     "grammar_production",
     "SBSQL-SURFACE-10BCEBE5B74F",
     "ALTER PRINCIPAL app_user DISABLE",
     "security.principal.alter",
     "SBLR_SECURITY_PRINCIPAL_ALTER",
     "security",
     "sblr.security.mutation.v3",
     "parser.statement_family.security",
     "lowering.sblr_family.sblr_security_mutation_v3",
     "server.admission.sblr_security_mutation_v3",
     "engine.rule.sblr_security_mutation_v3"},
    {"SBSQL-F15CCA3D7F79",
     "create_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-7C8CE49C0168",
     "CREATE POLICY app_policy ON TABLE customer",
     "security.policy.create",
     "SBLR_SECURITY_POLICY_CREATE",
     "security",
     "sblr.policy.operation.v3",
     "parser.statement_family.security",
     "lowering.sblr_family.sblr_policy_operation_v3",
     "server.admission.sblr_policy_operation_v3",
     "engine.rule.sblr_policy_operation_v3"},
    {"SBSQL-D0C7A3336A8B",
     "alter_policy_stmt",
     "grammar_production",
     "SBSQL-SURFACE-7DBBEB076EA0",
     "ALTER POLICY app_policy SET INACTIVE",
     "security.policy.alter",
     "SBLR_SECURITY_POLICY_ALTER",
     "security",
     "sblr.policy.operation.v3",
     "parser.statement_family.security",
     "lowering.sblr_family.sblr_policy_operation_v3",
     "server.admission.sblr_policy_operation_v3",
     "engine.rule.sblr_policy_operation_v3"},
    {"SBSQL-54D40FB44CD0",
     "alter_security_action",
     "grammar_production",
     "SBSQL-SURFACE-8C9640774944",
     "ALTER POLICY app_policy SET INACTIVE",
     "security.policy.alter",
     "SBLR_SECURITY_POLICY_ALTER",
     "security",
     "sblr.policy.operation.v3",
     "parser.statement_family.security",
     "lowering.sblr_family.sblr_policy_operation_v3",
     "server.admission.sblr_policy_operation_v3",
     "engine.rule.sblr_policy_operation_v3"},
    {"SBSQL-05E7A34BFCA4",
     "principal_attribute",
     "grammar_production",
     "SBSQL-SURFACE-0B0E8DC80FEB",
     "CREATE PRINCIPAL app_user TYPE USER CREDENTIAL REF protected_ref_001",
     "security.principal.create",
     "SBLR_SECURITY_PRINCIPAL_CREATE",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-2430353C00E8",
     "privilege_set",
     "grammar_production",
     "SBSQL-SURFACE-C834F07F14C3",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
    {"SBSQL-DC56111DEE70",
     "privilege_name",
     "grammar_production",
     "SBSQL-SURFACE-BA2BAA725664",
     "GRANT SELECT ON customer TO app_role",
     "security.privilege.grant",
     "SBLR_SECURITY_PRIVILEGE_GRANT",
     "general",
     "sblr.general.operation.v3",
     "parser.grammar_ast",
     "lowering.sblr_family.sblr_general_operation_v3",
     "server.admission.sblr_general_operation_v3",
     "engine.rule.sblr_general_operation_v3"},
}};

std::string EvidenceMessage(const SecurityRowEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string_view ExpectedExecutableFamily(const SecurityRowEvidence& row) {
  if (StartsWith(row.operation_id, "security.policy.")) {
    return "sblr.policy.operation.v3";
  }
  if (row.operation_id == "security.privilege.grant" ||
      row.operation_id == "security.privilege.revoke" ||
      row.operation_id == "security.session.set_role" ||
      row.operation_id == "security.principal.create" ||
      row.operation_id == "security.principal.alter") {
    return "sblr.security.mutation.v3";
  }
  return row.source_sblr_operation_family;
}

std::string_view ExpectedRegistrySblrOperationFamily(const SecurityRowEvidence& row) {
  if (!row.source_sblr_operation_family.empty()) {
    return row.source_sblr_operation_family;
  }
  return ExpectedExecutableFamily(row);
}

std::string_view FamilyKeySuffix(std::string_view family) {
  if (family == "sblr.policy.operation.v3") return "sblr_policy_operation_v3";
  if (family == "sblr.security.mutation.v3") return "sblr_security_mutation_v3";
  if (family == "sblr.general.operation.v3") return "sblr_general_operation_v3";
  return "";
}

std::string ExpectedRouteKey(const SecurityRowEvidence& row,
                             std::string_view prefix,
                             std::string_view explicit_key) {
  if (!explicit_key.empty()) return std::string(explicit_key);
  if (prefix == "lowering") {
    return std::string(prefix) + ".sblr_family." +
           std::string(FamilyKeySuffix(ExpectedRegistrySblrOperationFamily(row)));
  }
  return std::string(prefix) + "." +
         std::string(FamilyKeySuffix(ExpectedRegistrySblrOperationFamily(row)));
}

std::vector<std::string> ResolvedUuidsFor(const SecurityRowEvidence& row) {
  if (row.operation_id == "security.privilege.grant" ||
      row.operation_id == "security.privilege.revoke") {
    return {std::string(kTargetUuid), std::string(kGranteeUuid)};
  }
  if (row.operation_id == "security.session.set_role") {
    return {std::string(kRoleUuid)};
  }
  if (row.operation_id == "security.principal.create" ||
      row.operation_id == "security.principal.alter") {
    return {std::string(kUserUuid)};
  }
  if (row.operation_id == "security.policy.create") {
    return {std::string(kPolicyUuid), std::string(kTargetUuid)};
  }
  if (row.operation_id == "security.policy.attach") {
    if (Contains(row.sql, "TO USER")) {
      return {std::string(kPolicyUuid), std::string(kUserUuid)};
    }
    if (Contains(row.sql, "EVENT TRIGGER")) {
      return {std::string(kPolicyUuid), std::string(kEventTriggerUuid)};
    }
    return {std::string(kPolicyUuid), std::string(kPolicyTargetUuid), std::string(kRoleUuid)};
  }
  return {std::string(kPolicyUuid)};
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000002311";
  session.connection_uuid = "019f0000-0000-7000-8000-000000002312";
  session.database_uuid = "019f0000-0000-7000-8000-000000002313";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000002314";
  config.bundle_contract_id = "sbp_sbsql@security-route-test";
  config.build_id = "sbsql-security-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql, std::vector<std::string> resolved = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, resolved);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence(const SecurityRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == row.surface_kind,
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == row.registry_family,
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now" ||
              registry_row->source_status == "e2e_passed",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == ExpectedRegistrySblrOperationFamily(row),
          EvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == row.parser_handler_key,
          EvidenceMessage(row, "parser_bind_lower", "parser handler key mismatch"));
  Require(std::string(registry_row->lowering_handler_key) ==
              ExpectedRouteKey(row, "lowering", row.lowering_handler_key),
          EvidenceMessage(row, "parser_bind_lower", "lowering handler key mismatch"));
  Require(std::string(registry_row->server_admission_key) ==
              ExpectedRouteKey(row, "server.admission", row.server_admission_key),
          EvidenceMessage(row, "server_admission", "server admission key mismatch"));
  Require(std::string(registry_row->engine_rule_key) ==
              ExpectedRouteKey(row, "engine.rule", row.engine_rule_key),
          EvidenceMessage(row, "engine_dispatch", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(const SecurityRowEvidence& row) {
  const auto artifacts = RunPipeline(row.sql, ResolvedUuidsFor(row));
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower",
                          "security statement did not bind after UUID resolution"));
  if (!artifacts.verifier.admitted) {
    for (const auto& diagnostic : artifacts.verifier.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.name << '=' << field.value << '\n';
      }
    }
  }
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower",
                          "security SBLR verifier rejected exact route"));
  const auto expected_family = ExpectedExecutableFamily(row);
  Require(artifacts.envelope.operation_family == expected_family,
          EvidenceMessage(row, "parser_bind_lower", "security operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == expected_family,
          EvidenceMessage(row, "parser_bind_lower", "security SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "security operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "security engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "security opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_rights, "right.security_admin"),
          EvidenceMessage(row, "parser_bind_lower", "security admin right missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_security_authorization"),
          EvidenceMessage(row, "parser_bind_lower",
                          "parser no-security-authorization authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          EvidenceMessage(row, "parser_bind_lower",
                          "parser no-storage/finality authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "parser_bind_lower",
                          "parser no-SQL-execution authority step missing"));
  if (row.operation_id == "security.privilege.grant" ||
      row.operation_id == "security.privilege.revoke") {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.security_privilege_api_required"),
            EvidenceMessage(row, "parser_bind_lower",
                            "engine security privilege authority step missing"));
    Require(HasValue(artifacts.envelope.descriptor_refs, "sys.security.privilege_grant"),
            EvidenceMessage(row, "parser_bind_lower", "security privilege descriptor ref missing"));
    Require(Contains(artifacts.envelope.payload, std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
            EvidenceMessage(row, "parser_bind_lower", "target UUID missing from security payload"));
    Require(Contains(artifacts.envelope.payload, std::string("\"grantee_uuid\":\"") + std::string(kGranteeUuid) + "\""),
            EvidenceMessage(row, "parser_bind_lower", "grantee UUID missing from security payload"));
  } else if (row.operation_id == "security.session.set_role") {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.security_session_role_api_required"),
            EvidenceMessage(row, "parser_bind_lower",
                            "engine security session role authority step missing"));
    Require(HasValue(artifacts.envelope.descriptor_refs, "sys.security.role"),
            EvidenceMessage(row, "parser_bind_lower", "security role descriptor ref missing"));
    Require(Contains(artifacts.envelope.payload, std::string("\"role_uuid\":\"") + std::string(kRoleUuid) + "\""),
            EvidenceMessage(row, "parser_bind_lower", "role UUID missing from security payload"));
  } else if (StartsWith(row.operation_id, "security.principal.")) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.security_principal_api_required"),
            EvidenceMessage(row, "parser_bind_lower",
                            "engine security principal authority step missing"));
    Require(HasValue(artifacts.envelope.descriptor_refs, "sys.security.principal"),
            EvidenceMessage(row, "parser_bind_lower", "security principal descriptor ref missing"));
    Require(Contains(artifacts.envelope.payload, std::string("\"principal_uuid\":\"") + std::string(kUserUuid) + "\""),
            EvidenceMessage(row, "parser_bind_lower", "principal UUID missing from security payload"));
    if (row.operation_id == "security.principal.create") {
      Require(Contains(artifacts.envelope.payload, "\"principal_name\":\"app_user\""),
              EvidenceMessage(row, "parser_bind_lower", "principal name payload missing from create route"));
      Require(Contains(artifacts.envelope.payload, "\"principal_kind\":\"user\""),
              EvidenceMessage(row, "parser_bind_lower", "principal kind payload missing from create route"));
      Require(Contains(artifacts.envelope.payload,
                       "\"credential_protected_material_ref\":\"protected_ref_001\""),
              EvidenceMessage(row, "parser_bind_lower",
                              "protected material reference payload missing from create route"));
    } else {
      Require(Contains(artifacts.envelope.payload, "\"lifecycle_state\":\"disabled\""),
              EvidenceMessage(row, "parser_bind_lower", "principal lifecycle payload missing from alter route"));
    }
  } else if (StartsWith(row.operation_id, "security.policy.")) {
    Require(HasValue(artifacts.envelope.required_authority_steps,
                     "authority.engine.security_policy_api_required"),
            EvidenceMessage(row, "parser_bind_lower",
                            "engine security policy authority step missing"));
    Require(HasValue(artifacts.envelope.descriptor_refs, "sys.security.policy"),
            EvidenceMessage(row, "parser_bind_lower", "security policy descriptor ref missing"));
    Require(Contains(artifacts.envelope.payload, std::string("\"policy_uuid\":\"") + std::string(kPolicyUuid) + "\""),
            EvidenceMessage(row, "parser_bind_lower", "policy UUID missing from security payload"));
    if (row.operation_id == "security.policy.create") {
      Require(Contains(artifacts.envelope.payload,
                       std::string("\"target_object_uuid\":\"") + std::string(kTargetUuid) + "\""),
              EvidenceMessage(row, "parser_bind_lower",
                              "policy create target UUID missing from security payload"));
      Require(Contains(artifacts.envelope.payload, "\"policy_name\":\"app_policy\""),
              EvidenceMessage(row, "parser_bind_lower",
                              "policy name payload missing from create route"));
      Require(Contains(artifacts.envelope.payload, "\"policy_effect\":\"row_filter\""),
              EvidenceMessage(row, "parser_bind_lower", "policy effect missing from create payload"));
    } else if (row.operation_id == "security.policy.alter") {
      Require(Contains(artifacts.envelope.payload, "\"lifecycle_state\":\"inactive\""),
              EvidenceMessage(row, "parser_bind_lower", "policy lifecycle payload missing from alter route"));
    } else if (row.operation_id == "security.policy.attach") {
      const std::string expected_target = Contains(row.sql, "TO USER")
          ? std::string(kUserUuid)
          : Contains(row.sql, "EVENT TRIGGER") ? std::string(kEventTriggerUuid)
                                               : std::string(kPolicyTargetUuid);
      Require(Contains(artifacts.envelope.payload,
                       std::string("\"target_object_uuid\":\"") + expected_target + "\""),
              EvidenceMessage(row, "parser_bind_lower",
                              "policy target UUID missing from security payload"));
    }
  }
  Require(!Contains(artifacts.envelope.payload, row.sql),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded source SQL text"));
  Require(!Contains(artifacts.envelope.payload, "customer"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded target name text"));
  Require(!Contains(artifacts.envelope.payload, "app_role"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded grantee name text"));
  if (row.operation_id != "security.policy.create") {
    Require(!Contains(artifacts.envelope.payload, "app_policy"),
            EvidenceMessage(row, "no_sql_text_authority",
                            "security envelope embedded policy name text"));
  }
  Require(!Contains(artifacts.envelope.payload, "placement_policy"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded placement policy name text"));
  if (row.operation_id != "security.principal.create") {
    Require(!Contains(artifacts.envelope.payload, "app_user"),
            EvidenceMessage(row, "no_sql_text_authority",
                            "security envelope embedded user name text"));
  }
  Require(!Contains(artifacts.envelope.payload, "audit_trigger"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded event trigger name text"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope embedded source_text"));
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":true"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "security envelope marked SQL text present"));

}

void RequireCanonicalSecurityRootRegistry() {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kRoots{{
      {"engine.op.sec_grant", "SBLR_SEC_GRANT"},
      {"engine.op.sec_revoke", "SBLR_SEC_REVOKE"},
      {"engine.op.session_role_switch", "SBLR_SESSION_ROLE_SWITCH"},
      {"engine.op.sec_create_policy", "SBLR_SEC_CREATE_POLICY"},
      {"engine.op.sec_alter_policy", "SBLR_SEC_ALTER_POLICY"},
      {"engine.op.catalog_introspect", "SBLR_CATALOG_INTROSPECT"},
  }};
  for (const auto& [operation_id, opcode] : kRoots) {
    const auto* entry = sblr::LookupSblrOperation(operation_id);
    Require(entry != nullptr, "canonical security root is missing from the opcode registry");
    Require(entry->opcode == opcode,
            "canonical security root has the wrong opcode mnemonic");
    Require(entry->code != 0,
            "canonical security root has no allocated numeric opcode");
  }
}

void RequireLegacyParserRootsRemainNonExecutable() {
  for (const auto& row : kSecurityRows) {
    const auto* entry = sblr::LookupSblrOperation(row.operation_id);
    Require(entry != nullptr,
            EvidenceMessage(row, "canonical_boundary",
                            "legacy parser route is absent from the compatibility registry"));
    if (row.operation_id != "security.policy.show") {
      Require(entry->code == 0,
              EvidenceMessage(row, "canonical_boundary",
                              "legacy parser route unexpectedly acquired canonical opcode authority"));
    }
  }
}

void RequireUnresolvedNamesFailClosed() {
  const auto artifacts = RunPipeline("GRANT SELECT ON customer TO app_role");
  Require(!artifacts.bound.bound || artifacts.envelope.messages.has_errors() ||
              artifacts.verifier.messages.has_errors(),
          "security DCL without UUID resolution did not fail closed");
}

}  // namespace

int main() {
  for (const auto& row : kSecurityRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
  }
  RequireUnresolvedNamesFailClosed();
  RequireCanonicalSecurityRootRegistry();
  RequireLegacyParserRootsRemainNonExecutable();
  std::cout << "sbsql_security_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
