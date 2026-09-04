// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "extensibility/udr_api.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sb_udr_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace sblr = scratchbird::engine::sblr;
namespace udr_runtime = scratchbird::udr::runtime;

constexpr std::string_view kPackageUuid = "019f0000-0000-7000-8000-000000003901";
constexpr std::string_view kPackageName = "sbup_demo";
constexpr std::string_view kDatabasePath =
    "/tmp/sbsql_udr_package_management_exact_route_conformance.sbdb";

std::string g_database_uuid = "019f0000-0000-7000-8000-000000003801";
std::uint64_t g_local_transaction_id = 0;
scratchbird::engine::internal_api::EngineUuid g_transaction_uuid;
std::uint64_t g_snapshot_visible_through_local_transaction_id = 0;
std::string g_transaction_isolation_level;

struct UdrRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
  std::string_view expected_field;
  std::string_view expected_value;
  bool contains{false};
};

struct UdrLifecycleRouteCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view surface_variant;
  bool mutation;
};

constexpr std::array<UdrRowEvidence, 6> kUdrRows{{
    {"SBSQL-F0CF86A4B3AF",
     "udr_package_stmt",
     "SBSQL-SURFACE-9084358EE3A1",
     "object_kind",
     "udr_package",
     false},
    {"SBSQL-A3D801F6079D",
     "udr_package_name",
     "SBSQL-SURFACE-09C11F5E5BD4",
     "name",
     "sbup_demo",
     false},
    {"SBSQL-C3138AC0D3EA",
     "udr_name",
     "SBSQL-SURFACE-6D67C6B15DBF",
     "name",
     "sbup_demo",
     false},
    {"SBSQL-202B0DD6C682",
     "udr_binary_ref",
     "SBSQL-SURFACE-365AB3C05772",
     "binary_hash",
     "sha256:sbup-demo",
     false},
    {"SBSQL-2FA96214E399",
     "udr_capability",
     "SBSQL-SURFACE-5FC597942BFA",
     "capability_role",
     "parser_support",
     false},
    {"SBSQL-7152B9A9B751",
     "udr_entry_point",
     "SBSQL-SURFACE-AA7130CCC0F5",
     "entrypoints",
     "sb_udr_demo_entry",
    true},
}};

constexpr std::array<UdrLifecycleRouteCase, 7> kLifecycleRoutes{{
    {"INSPECT UDR PACKAGE sbup_demo",
     "extensibility.inspect_udr_packages",
     "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES",
     "inspect_udr_package",
     false},
    {"CREATE UDR PACKAGE sbup_demo LANGUAGE C++ ABI VERSION 'sb_udr_v1' BINARY 'sha256:sbup-demo' WITH ENTRY POINTS (sb_udr_demo_entry)",
     "extensibility.register_udr_package",
     "SBLR_EXTENSIBILITY_REGISTER_UDR_PACKAGE",
     "create_udr_package",
     true},
    {"ALTER UDR PACKAGE sbup_demo SET SIGNATURE 'trusted-test-signature'",
     "extensibility.alter_udr_package",
     "SBLR_EXTENSIBILITY_ALTER_UDR_PACKAGE",
     "alter_udr_package",
     true},
    {"LOAD UDR PACKAGE sbup_demo",
     "extensibility.load_udr_package",
     "SBLR_EXTENSIBILITY_LOAD_UDR_PACKAGE",
     "load_udr_package",
     true},
    {"UNLOAD UDR PACKAGE sbup_demo",
     "extensibility.unload_udr_package",
     "SBLR_EXTENSIBILITY_UNLOAD_UDR_PACKAGE",
     "unload_udr_package",
     true},
    {"DROP UDR PACKAGE sbup_demo",
     "extensibility.drop_udr_package",
     "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE",
     "drop_udr_package",
     true},
    {"UNINSTALL UDR PACKAGE sbup_demo",
     "extensibility.drop_udr_package",
     "SBLR_EXTENSIBILITY_DROP_UDR_PACKAGE",
     "uninstall_udr_package",
     true},
}};

std::string EvidenceMessage(const UdrRowEvidence& row,
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

std::string RouteMessage(const UdrLifecycleRouteCase& route,
                         std::string_view phase,
                         std::string_view message) {
  std::string rendered(route.surface_variant);
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

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(false, message);
  }
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view key) {
  for (const auto& [field_name, field_value] : row.fields) {
    if (field_name == key) return field_value.encoded_value;
  }
  return {};
}

bool ApiResultHasField(const api::EngineApiResult& result,
                       std::string_view name,
                       std::string_view value,
                       bool contains) {
  for (const auto& row : result.result_shape.rows) {
    const std::string field = FieldValue(row, name);
    if (contains ? Contains(field, value) : field == value) return true;
  }
  return false;
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
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
  session.session_uuid = "019f0000-0000-7000-8000-000000003701";
  session.connection_uuid = "019f0000-0000-7000-8000-000000003702";
  session.database_uuid = "019f0000-0000-7000-8000-000000003703";
  session.catalog_epoch = 43;
  session.security_policy_epoch = 47;
  session.descriptor_epoch = 53;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-000000003704";
  config.bundle_contract_id = "sbp_sbsql@udr-package-management-route-test";
  config.build_id = "sbsql-udr-package-management-route-test";
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

void RequireRegistryEvidence(const UdrRowEvidence& row) {
  const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == "grammar_production",
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "runtime_management",
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.management.runtime_operation.v3",
          EvidenceMessage(row, "registry", "canonical SBLR operation family mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void RequireExactLowering(const UdrRowEvidence& row) {
  const auto artifacts = RunPipeline("SHOW UDR PACKAGES");
  Require(artifacts.bound.bound,
          EvidenceMessage(row, "parser_bind_lower", "UDR package statement did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "UDR inspect route required name registry resolution"));
  Require(!artifacts.bound.requires_transaction_authority,
          EvidenceMessage(row, "parser_bind_lower", "parser owned UDR transaction authority"));
  Require(artifacts.bound.statement_parser_category == "runtime_management",
          EvidenceMessage(row, "parser_bind_lower", "UDR route did not bind as runtime management"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "UDR SBLR verifier rejected exact route"));
  Require(artifacts.envelope.operation_family == "sblr.udr.operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == "sblr.udr.operation.v3",
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == "SBLR_EXTENSIBILITY_INSPECT_UDR_PACKAGES",
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.udr_inspect_api_required"),
          EvidenceMessage(row, "parser_bind_lower", "UDR inspect authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.transaction_context_required"),
          EvidenceMessage(row, "parser_bind_lower", "UDR transaction context handoff missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_udr_execution"),
          EvidenceMessage(row, "parser_bind_lower", "parser no-UDR-execution step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "no_sql_text_authority",
                          "parser no-SQL-execution authority step missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_package_registry"),
          EvidenceMessage(row, "parser_bind_lower", "UDR package registry descriptor missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_runtime_descriptor"),
          EvidenceMessage(row, "parser_bind_lower", "UDR runtime descriptor missing"));
  Require(HasValue(artifacts.envelope.required_rights, "right.udr_inspect"),
          EvidenceMessage(row, "parser_bind_lower", "required right mismatch"));
  Require(!artifacts.envelope.parser_executes_sql,
          EvidenceMessage(row, "no_sql_engine_execution",
                          "UDR lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          EvidenceMessage(row, "no_reference_execution",
                          "UDR lowering allowed parser file effects"));
  Require(!Contains(artifacts.envelope.payload, "SHOW UDR PACKAGES"),
          EvidenceMessage(row, "no_sql_text_authority", "UDR envelope embedded source SQL"));
  Require(Contains(artifacts.envelope.payload,
                   "\"udr_envelope_kind\":\"udr_package_inspect\""),
          EvidenceMessage(row, "parser_bind_lower", "UDR envelope kind missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"runtime_component\":\"udr_packages\""),
          EvidenceMessage(row, "parser_bind_lower", "UDR runtime component missing"));
  Require(Contains(artifacts.envelope.payload,
                   "\"parser_executes_udr\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser UDR execution denial missing"));
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          EvidenceMessage(row, "parser_bind_lower", "row surface id missing from UDR payload"));

  const auto* opcode =
      sblr::LookupSblrOperation("extensibility.inspect_udr_packages");
  Require(opcode != nullptr && opcode->code == 0,
          EvidenceMessage(row, "parser_component",
                          "unallocated UDR inspect route acquired canonical opcode authority"));
}

void RequireLifecycleLowering(const UdrLifecycleRouteCase& route) {
  const auto artifacts = RunPipeline(route.sql);
  if (!artifacts.bound.bound) {
    std::cerr << "route_sql=" << route.sql << '\n'
              << "ast_surface=" << artifacts.ast.statement_surface_name << '\n'
              << "ast_category=" << artifacts.ast.statement_parser_category << '\n'
              << "ast_requires_name_resolution="
              << (artifacts.ast.requires_name_resolution ? "true" : "false") << '\n'
              << "bound_surface=" << artifacts.bound.statement_surface_name << '\n'
              << "bound_category=" << artifacts.bound.statement_parser_category << '\n'
              << "bound_requires_name_resolution="
              << (artifacts.bound.requires_name_resolution ? "true" : "false") << '\n';
    for (const auto& diagnostic : artifacts.bound.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.severity << ':'
                << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.name << '=' << field.value << '\n';
      }
    }
  }
  Require(artifacts.bound.bound,
          RouteMessage(route, "parser_bind_lower", "UDR lifecycle route did not bind"));
  if (!artifacts.verifier.admitted) {
    std::cerr << "route_sql=" << route.sql << '\n'
              << "operation_id=" << artifacts.envelope.operation_id << '\n'
              << "operation_family=" << artifacts.envelope.operation_family << '\n'
              << "sblr_operation_key=" << artifacts.envelope.sblr_operation_key << '\n'
              << "opcode=" << artifacts.envelope.sblr_opcode << '\n'
              << "payload=" << artifacts.envelope.payload << '\n';
    for (const auto& diagnostic : artifacts.verifier.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.severity << ':'
                << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields) {
        std::cerr << "  " << field.name << '=' << field.value << '\n';
      }
    }
  }
  Require(artifacts.verifier.admitted,
          RouteMessage(route, "parser_bind_lower", "UDR lifecycle verifier rejected route"));
  Require(artifacts.envelope.operation_family == "sblr.udr.operation.v3",
          RouteMessage(route, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.operation_id == route.operation_id,
          RouteMessage(route, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.engine_api_operation_id == route.operation_id,
          RouteMessage(route, "parser_bind_lower", "engine API operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == route.opcode,
          RouteMessage(route, "parser_bind_lower", "opcode mismatch"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   route.mutation ? "authority.engine.udr_manage_api_required"
                                  : "authority.engine.udr_inspect_api_required"),
          RouteMessage(route, "parser_bind_lower", "UDR authority step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_udr_execution"),
          RouteMessage(route, "parser_bind_lower", "parser no-UDR-execution step missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          RouteMessage(route, "parser_bind_lower", "parser no-SQL-execution step missing"));
  Require(HasValue(artifacts.envelope.required_rights,
                   route.mutation ? "right.udr_manage" : "right.udr_inspect"),
          RouteMessage(route, "parser_bind_lower", "required right mismatch"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_package_registry"),
          RouteMessage(route, "parser_bind_lower", "UDR package registry descriptor missing"));
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.udr_runtime_descriptor"),
          RouteMessage(route, "parser_bind_lower", "UDR runtime descriptor missing"));
  Require(!artifacts.envelope.parser_executes_sql,
          RouteMessage(route, "no_sql_engine_execution",
                       "UDR lifecycle lowering allowed parser SQL execution"));
  Require(!artifacts.envelope.real_file_effects,
          RouteMessage(route, "no_file_effects",
                       "UDR lifecycle lowering allowed parser file effects"));
  Require(!Contains(artifacts.envelope.payload, route.sql),
          RouteMessage(route, "no_sql_text_authority",
                       "UDR lifecycle envelope embedded source SQL"));
  Require(Contains(artifacts.envelope.payload,
                   route.mutation ? "\"udr_envelope_kind\":\"udr_package_lifecycle\""
                                  : "\"udr_envelope_kind\":\"udr_package_inspect\""),
          RouteMessage(route, "parser_bind_lower", "UDR envelope kind missing"));
  Require(Contains(artifacts.envelope.payload, route.surface_variant),
          RouteMessage(route, "parser_bind_lower", "surface variant missing from payload"));
  Require(Contains(artifacts.envelope.payload, "\"udr_package_name\":\"sbup_demo\""),
          RouteMessage(route, "parser_bind_lower", "UDR package name missing from payload"));

  const auto* opcode = sblr::LookupSblrOperation(route.operation_id);
  Require(opcode != nullptr && opcode->code == 0,
          RouteMessage(route, "parser_component",
                       "unallocated UDR lifecycle route acquired canonical opcode authority"));
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-udr-package-management-exact-route";
  context.security_context_present = true;
  context.trace_tags.push_back("right:UDR_MANAGE");
  context.trace_tags.push_back("right:UDR_INSPECT");
  context.database_path = std::string(kDatabasePath);
  context.database_uuid.canonical = g_database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000003802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000003803";
  if (g_local_transaction_id != 0) {
    context.transaction_uuid = g_transaction_uuid;
    context.local_transaction_id = g_local_transaction_id;
    context.snapshot_visible_through_local_transaction_id =
        g_snapshot_visible_through_local_transaction_id;
    context.transaction_isolation_level = g_transaction_isolation_level;
  }
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 43;
  context.security_epoch = 47;
  context.resource_epoch = 53;
  context.name_resolution_epoch = 59;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7000-8000-000000003805";
  context.authorization_context.security_context_generation = 1;
  context.authorization_context.principal_uuid = context.principal_uuid;
  context.authorization_context.security_epoch = context.security_epoch;
  context.authorization_context.policy_epoch = context.resource_epoch;
  context.authorization_context.catalog_generation_id =
      context.catalog_generation_id;
  context.authorization_context.effective_subjects.push_back(
      {context.principal_uuid, "principal"});
  api::EngineMaterializedAuthorizationGrant manage;
  manage.grant_uuid.canonical =
      "019f0000-0000-7000-8000-000000003806";
  manage.subject_uuid = context.principal_uuid;
  manage.subject_kind = "principal";
  manage.right = "UDR_MANAGE";
  manage.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(manage));
  api::EngineMaterializedAuthorizationGrant inspect;
  inspect.grant_uuid.canonical =
      "019f0000-0000-7000-8000-000000003807";
  inspect.subject_uuid = context.principal_uuid;
  inspect.subject_kind = "principal";
  inspect.right = "UDR_INSPECT";
  inspect.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(inspect));
  return context;
}

std::uint64_t NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

platform::TypedUuid Generate(platform::UuidKind kind, std::uint64_t millis) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : platform::TypedUuid{};
}

void CreateRouteDatabase() {
  std::remove(std::string(kDatabasePath).c_str());
  std::remove((std::string(kDatabasePath) + ".sb.owner.lock").c_str());
  std::remove((std::string(kDatabasePath) + ".sb.api_events").c_str());
  std::remove((std::string(kDatabasePath) + ".sb.crud_events").c_str());

  scratchbird::storage::database::DatabaseCreateConfig create;
  const auto seed = NowMillis();
  create.path = std::string(kDatabasePath);
  create.database_uuid = Generate(platform::UuidKind::database, seed);
  create.filespace_uuid = Generate(platform::UuidKind::filespace, seed + 1);
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  Require(create.database_uuid.valid() && create.filespace_uuid.valid(),
          "failed to generate database/filespace UUIDs for UDR route test");
  Require(scratchbird::storage::database::CreateDatabaseFile(create).ok(),
          "failed to create database for UDR route test");
  g_database_uuid = uuid::UuidToString(create.database_uuid.value);
}

void BeginRouteTransaction() {
  api::EngineBeginTransactionRequest begin;
  begin.context = EngineContext();
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "failed to begin MGA transaction for UDR route test");
  g_local_transaction_id = begun.local_transaction_id;
  g_transaction_uuid = begun.transaction_uuid;
  g_snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  g_transaction_isolation_level = begun.isolation_level;
}

api::EngineLocalizedName LocalizedName(std::string name) {
  api::EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "primary";
  localized.path = "sys.udr";
  localized.name = std::move(name);
  localized.default_name = true;
  return localized;
}

udr_runtime::UdrCallResult DemoEntrypoint(const udr_runtime::UdrCallInput&) {
  udr_runtime::UdrCallResult result;
  result.ok = true;
  result.payload = "{\"ok\":true}";
  result.message_vector_json = "{\"diagnostics\":[]}";
  return result;
}

udr_runtime::UdrPackageDescriptor DemoDescriptor() {
  udr_runtime::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = std::string(kPackageUuid);
  descriptor.package_name = std::string(kPackageName);
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "src-rev-sbup-demo";
  descriptor.binary_hash = "sha256:sbup-demo";
  descriptor.signature_policy = "trusted-test-signature";
  descriptor.capability_role = "parser_support";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints.push_back({"sb_udr_demo_entry", "parser_support", DemoEntrypoint});
  return descriptor;
}

void AddManageUdrOptions(api::EngineApiRequest* request,
                         const udr_runtime::UdrPackageDescriptor& descriptor) {
  request->option_envelopes.push_back("permission:manage_udr");
  request->option_envelopes.push_back("trusted_cpp_udr");
  request->option_envelopes.push_back("abi:sb_udr_v1");
  request->option_envelopes.push_back("name:" + descriptor.package_name);
  request->option_envelopes.push_back("linked_udr_package:true");
  request->option_envelopes.push_back("source_revision:" + descriptor.source_revision);
  request->option_envelopes.push_back("binary_hash:" + descriptor.binary_hash);
  request->option_envelopes.push_back("signature_policy:" + descriptor.signature_policy);
  request->option_envelopes.push_back("capability_role:" + descriptor.capability_role);
}

void SeedActiveTransaction() {
  std::ofstream out(std::string(kDatabasePath) + ".sb.crud_events",
                    std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t" << g_local_transaction_id
      << "\tsbsql_udr_package_management_exact_route\n";
  Require(static_cast<bool>(out), "failed to seed MGA transaction evidence for UDR route test");
}

void RegisterDemoPackage() {
  const auto descriptor = DemoDescriptor();
  const auto runtime_registered = udr_runtime::RegisterPackage(descriptor);
  Require(runtime_registered.ok, "failed to seed UDR runtime package descriptor");
  SeedActiveTransaction();

  api::EngineRegisterUdrPackageRequest request;
  request.context = EngineContext();
  request.target_database.uuid.canonical = g_database_uuid;
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = descriptor.package_uuid;
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back(LocalizedName(descriptor.package_name));
  AddManageUdrOptions(&request, descriptor);
  const auto registered = api::EngineRegisterUdrPackage(request);
  for (const auto& diagnostic : registered.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(registered.ok, "engine refused trusted C++ UDR package registration");
  Require(ApiResultHasEvidence(registered, "udr_descriptor", "runtime_descriptor_validated"),
          "UDR registration did not validate the runtime descriptor");
  Require(ApiResultHasEvidence(registered, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          "UDR registration did not preserve MGA/SBLR/UUID/security authority evidence");
}

template <typename TRequest>
TRequest UdrRequest(const udr_runtime::UdrPackageDescriptor& descriptor) {
  TRequest request;
  request.context = EngineContext();
  request.target_database.uuid.canonical = g_database_uuid;
  request.target_database.object_kind = "database";
  request.target_object.uuid.canonical = descriptor.package_uuid;
  request.target_object.object_kind = "udr_package";
  request.localized_names.push_back(LocalizedName(descriptor.package_name));
  AddManageUdrOptions(&request, descriptor);
  return request;
}

void RequireLifecycleEngineApis() {
  const auto descriptor = DemoDescriptor();

  auto alter_request = UdrRequest<api::EngineAlterUdrPackageRequest>(descriptor);
  alter_request.option_envelopes.push_back("alter_action:set_signature");
  const auto altered = api::EngineAlterUdrPackage(alter_request);
  for (const auto& diagnostic : altered.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(altered.ok, "engine refused trusted C++ UDR package alter");
  Require(ApiResultHasEvidence(altered, "udr_catalog", "uuid_identity_preserved"),
          "UDR alter did not preserve UUID catalog identity");
  Require(ApiResultHasEvidence(altered, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          "UDR alter did not preserve authority-boundary evidence");

  const auto loaded = api::EngineLoadUdrPackage(
      UdrRequest<api::EngineLoadUdrPackageRequest>(descriptor));
  for (const auto& diagnostic : loaded.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(loaded.ok, "engine refused trusted C++ UDR package load");
  Require(ApiResultHasEvidence(loaded, "udr_entrypoints", "dispatch_table_published"),
          "UDR load did not publish entrypoint evidence");

  const auto unloaded = api::EngineUnloadUdrPackage(
      UdrRequest<api::EngineUnloadUdrPackageRequest>(descriptor));
  for (const auto& diagnostic : unloaded.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(unloaded.ok, "engine refused trusted C++ UDR package unload");
  Require(ApiResultHasEvidence(unloaded, "udr_entrypoints", "dispatch_table_removed"),
          "UDR unload did not remove entrypoint evidence");

  const auto dropped = api::EngineDropUdrPackage(
      UdrRequest<api::EngineDropUdrPackageRequest>(descriptor));
  for (const auto& diagnostic : dropped.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(dropped.ok, "engine refused trusted C++ UDR package drop");
  Require(ApiResultHasEvidence(dropped, "udr_loader", "runtime_descriptor_unregistered"),
          "UDR drop did not unregister runtime descriptor");
  Require(ApiResultHasEvidence(dropped, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          "UDR drop did not preserve authority-boundary evidence");
  Require(!udr_runtime::GetPackageState(descriptor.package_uuid).has_value(),
          "UDR drop left runtime package state registered");

  api::EngineInspectUdrPackageRequest inspect;
  inspect.context = EngineContext();
  inspect.option_envelopes.push_back("permission:inspect_udr");
  const auto inspected = api::EngineInspectUdrPackages(inspect);
  Require(inspected.ok, "UDR inspect after drop failed");
  Require(!ApiResultHasField(inspected, "object_uuid", descriptor.package_uuid, false),
          "dropped UDR package remained visible");
}

void RequireEngineApiInspection(const UdrRowEvidence& row) {
  api::EngineInspectUdrPackageRequest request;
  request.context = EngineContext();
  request.option_envelopes.push_back("permission:inspect_udr");
  const auto result = api::EngineInspectUdrPackages(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.ok,
          EvidenceMessage(row, "engine_api", "engine UDR inspect API returned failure"));
  Require(result.operation_id == "extensibility.inspect_udr_packages",
          EvidenceMessage(row, "engine_api", "engine API operation id mismatch"));
  Require(ApiResultHasEvidence(result, "extension_behavior", "inspected"),
          EvidenceMessage(row, "engine_api", "UDR inspect evidence missing"));
  Require(ApiResultHasEvidence(result, "authority_boundary",
                               "mga_sblr_uuid_security_transaction_preserved"),
          EvidenceMessage(row, "engine_api", "authority-boundary evidence missing"));
  Require(ApiResultHasField(result,
                            row.expected_field,
                            row.expected_value,
                            row.contains),
          EvidenceMessage(row, "engine_api", "expected UDR result field missing"));
}

}  // namespace

int main() {
  udr_runtime::ResetRuntimeForTest();
  CreateRouteDatabase();
  BeginRouteTransaction();
  RegisterDemoPackage();
  for (const auto& route : kLifecycleRoutes) {
    RequireLifecycleLowering(route);
  }
  for (const auto& row : kUdrRows) {
    RequireRegistryEvidence(row);
    RequireExactLowering(row);
    RequireEngineApiInspection(row);
  }
  RequireLifecycleEngineApis();
  std::cout << "sbsql_udr_package_management_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
