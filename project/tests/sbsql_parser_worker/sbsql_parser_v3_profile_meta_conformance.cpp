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
#include "meta/meta_command_surface.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

sbsql::SessionContext AuthenticatedSession() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "session-profile-meta";
  session.connection_uuid = "connection-profile-meta";
  session.database_uuid = "019e17c1-0000-7000-8000-000000000001";
  session.authenticated_user_uuid = "019e17c1-0000-7000-8000-000000000002";
  session.dialect_profile_uuid = "sbsql.default";
  session.catalog_epoch = 3;
  session.security_policy_epoch = 5;
  session.descriptor_epoch = 7;
  return session;
}

void VerifyCstSourceBufferMetadata() {
  const auto cst = sbsql::BuildCst("select\r\n 1;\r\n");
  Require(cst.source == sbsql::ReconstructSourceFromTokens(cst),
          "CST source reconstruction is not lossless");
  Require(!cst.source_buffer_uuid.empty() &&
              cst.source_buffer_uuid.rfind("cst.source_buffer.", 0) == 0,
          "CST source buffer UUID evidence missing");
  Require(!cst.source_hash.empty(), "CST source hash missing");
  Require(cst.line_ending_mode == "crlf", "CST line-ending mode was not recorded");
  Require(cst.dialect_profile_uuid == "sbsql.default", "CST dialect profile missing");
  Require(cst.trivia_preserved, "CST trivia preservation flag missing");
}

void VerifyMetaCommandRegistryAndRefusals() {
  const auto records = sbsql::BuiltinMetaCommandSurfaceRecords();
  Require(records.size() >= 15, "meta-command surface registry is incomplete");
  const auto* shell = sbsql::ResolveMetaCommandSurface("\\!");
  Require(shell != nullptr && shell->refusal_diagnostic == "SBSQL.META.LOCAL_SHELL_REFUSED",
          "local shell meta-command did not resolve to exact refusal");
  Require(shell->parser_executes_local_process,
          "local shell record must document the forbidden local process effect");

  const auto* describe = sbsql::ResolveMetaCommandSurface("\\d");
  Require(describe != nullptr &&
              describe->surface_class == sbsql::MetaCommandSurfaceClass::kMetadataReport,
          "describe meta-command did not resolve to metadata report surface");
  Require(describe->disposition == sbsql::MetaCommandDisposition::kExactRefusal,
          "describe meta-command must fail closed without a reference/tool profile");

  const auto shell_ast = sbsql::BuildAst(sbsql::BuildCst("\\! echo unsafe"));
  Require(shell_ast.exact_refusal_required, "local shell AST did not require exact refusal");
  Require(HasDiagnostic(shell_ast.messages, "SBSQL.META.LOCAL_SHELL_REFUSED"),
          "local shell AST missing refusal diagnostic");
  Require(!shell_ast.produces_sblr, "refused meta-command produced SBLR");

  const auto describe_ast = sbsql::BuildAst(sbsql::BuildCst("\\d users.public.t"));
  Require(describe_ast.exact_refusal_required, "reference metadata AST did not require exact refusal");
  Require(HasDiagnostic(describe_ast.messages, "SBSQL.META.PROFILE_REQUIRED"),
          "reference metadata AST missing profile-required diagnostic");
}

void VerifyManagementAndClusterClassification() {
  const auto backup_ast = sbsql::BuildAst(sbsql::BuildCst("BACKUP DATABASE example;"));
  Require(backup_ast.family == sbsql::StatementFamily::kArchiveReplication,
          "BACKUP did not classify as archive/replication management");
  Require(backup_ast.operation_family == "sblr.archive_replication.operation.v3",
          "BACKUP did not map to archive/replication SBLR family");
  Require(!backup_ast.statement_surface_id.empty(), "BACKUP did not resolve a registry surface");

  const auto checkpoint_ast = sbsql::BuildAst(sbsql::BuildCst("CHECKPOINT;"));
  Require(checkpoint_ast.family == sbsql::StatementFamily::kStorageManagement,
          "CHECKPOINT did not classify as storage management");
  Require(checkpoint_ast.operation_family == "sblr.storage.management_operation.v3",
          "CHECKPOINT did not map to storage management SBLR family");

  const auto cluster_ast = sbsql::BuildAst(sbsql::BuildCst("CLUSTER TOPOLOGY SHOW;"));
  Require(cluster_ast.family == sbsql::StatementFamily::kClusterPrivate,
          "CLUSTER did not classify as private cluster surface");
  Require(cluster_ast.requires_cluster_profile && cluster_ast.exact_refusal_required,
          "CLUSTER surface was not fail-closed in public profile");
  Require(cluster_ast.diagnostic_key == "diagnostic.cluster_profile_fail_closed",
          "CLUSTER surface did not carry cluster refusal diagnostic key");

  sbsql::ParserConfig config;
  config.parser_uuid = "sbp_sbsql";
  const auto bound = sbsql::BindAst(cluster_ast, sbsql::BuildCst("CLUSTER TOPOLOGY SHOW;"),
                                    config, AuthenticatedSession(), {});
  Require(!bound.bound, "cluster-private statement bound without cluster authority");
  Require(HasDiagnostic(bound.messages, "SBSQL.CLUSTER.AUTHORITY_REQUIRED"),
          "cluster-private statement missing authority-required diagnostic");
}

void VerifyGeneratedRegistryProfiles() {
  std::size_t native_now = 0;
  std::size_t native_future = 0;
  std::size_t cluster_private = 0;
  for (const auto& row : sbsql::GeneratedSurfaceRegistryRows()) {
    if (row.source_status == "native_now") ++native_now;
    if (row.source_status == "native_future") ++native_future;
    if (row.cluster_scope == "cluster_private") ++cluster_private;
  }
  Require(native_now + native_future == 2546,
          "generated registry public source-status profile unexpectedly changed");
  Require(native_now == 2546, "generated registry native-now profile unexpectedly changed");
  Require(native_future == 0, "generated registry must not retain native-future inventory rows");
  Require(cluster_private > 20, "generated registry cluster-private profile unexpectedly small");
  Require(sbsql::FindGeneratedSurfaceRegistryRowByCanonicalName("backup_stmt") != nullptr,
          "generated registry missing backup management surface");
  Require(sbsql::FindGeneratedSurfaceRegistryRowByCanonicalName("cluster_stmt") != nullptr,
          "generated registry missing cluster-private surface");
}

} // namespace

int main() {
  VerifyCstSourceBufferMetadata();
  VerifyMetaCommandRegistryAndRefusals();
  VerifyManagementAndClusterClassification();
  VerifyGeneratedRegistryProfiles();
  std::cout << "sbsql_parser_v3_profile_meta_conformance=passed\n";
  return EXIT_SUCCESS;
}
