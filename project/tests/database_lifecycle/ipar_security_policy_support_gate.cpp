// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_security_policy_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace server = scratchbird::server;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ipar_security_policy_support_gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

server::IparSecurityPolicyEpochVector Epoch(std::uint64_t value) {
  server::IparSecurityPolicyEpochVector epoch;
  epoch.catalog_generation = value;
  epoch.security_epoch = value + 1;
  epoch.descriptor_epoch = value + 2;
  epoch.grant_epoch = value + 3;
  epoch.policy_generation = value + 4;
  epoch.resource_epoch = value + 5;
  epoch.cache_invalidation_epoch = value + 6;
  epoch.role_set_hash = "roles/ipar/" + std::to_string(value);
  epoch.group_set_hash = "groups/ipar/" + std::to_string(value);
  return epoch;
}

server::IparSecurityPolicySnapshotPut Snapshot(
    server::IparDmlDdlOperationClass operation_class,
    server::IparSecurityPolicyEpochVector epoch) {
  server::IparSecurityPolicySnapshotPut snapshot;
  snapshot.snapshot_id =
      operation_class == server::IparDmlDdlOperationClass::kDdl
          ? "ddl-security-snapshot"
          : "dml-security-snapshot";
  snapshot.database_uuid = "db-uuid";
  snapshot.object_uuid = operation_class == server::IparDmlDdlOperationClass::kDdl
                             ? "table-ddl-uuid"
                             : "table-dml-uuid";
  snapshot.principal_uuid = "principal-uuid";
  snapshot.auth_context_uuid = "auth-context-uuid";
  snapshot.operation_class = operation_class;
  snapshot.operation_id =
      operation_class == server::IparDmlDdlOperationClass::kDdl
          ? "ddl.alter_table"
          : "dml.insert";
  snapshot.epoch = std::move(epoch);
  snapshot.object_rights_mask = true;
  snapshot.rls_required = operation_class == server::IparDmlDdlOperationClass::kDml;
  snapshot.mask_required = operation_class == server::IparDmlDdlOperationClass::kDml;
  snapshot.ddl_policy_required =
      operation_class == server::IparDmlDdlOperationClass::kDdl;
  snapshot.column_rights.push_back(
      {"col-id", true, true, true, false,
       operation_class == server::IparDmlDdlOperationClass::kDdl});
  snapshot.column_rights.push_back(
      {"col-secret", true, false, false, false, false});
  return snapshot;
}

void ProveSecurityPolicySnapshotCache() {
  server::IparSecurityPolicySupportService service;
  const auto epoch = Epoch(10);
  const auto put = service.PutSecurityPolicySnapshot(
      Snapshot(server::IparDmlDdlOperationClass::kDml, epoch));
  Require(put.accepted && !put.fail_closed,
          "IPAR-P1-10 DML security snapshot should cache");
  Require(!put.record.rights_digest.empty(),
          "IPAR-P1-10 rights digest should be sealed");

  server::IparSecurityPolicyLookup lookup;
  lookup.cache_key = put.record.cache_key;
  lookup.database_uuid = "db-uuid";
  lookup.object_uuid = "table-dml-uuid";
  lookup.principal_uuid = "principal-uuid";
  lookup.auth_context_uuid = "auth-context-uuid";
  lookup.operation_class = server::IparDmlDdlOperationClass::kDml;
  lookup.operation_id = "dml.insert";
  lookup.epoch = epoch;
  auto hit = service.LookupSecurityPolicySnapshot(lookup);
  Require(hit.accepted && hit.cache_hit && !hit.fail_closed,
          "IPAR-P1-10 matching DML security snapshot should hit");

  lookup.epoch.grant_epoch += 1;
  const auto stale = service.LookupSecurityPolicySnapshot(lookup);
  Require(!stale.accepted && stale.stale && stale.fail_closed &&
              stale.detail == "grant_epoch_stale",
          "IPAR-P1-10 grant revoke epoch should stale-refuse cached authority");

  auto ddl_put = service.PutSecurityPolicySnapshot(
      Snapshot(server::IparDmlDdlOperationClass::kDdl, Epoch(30)));
  Require(ddl_put.accepted && !ddl_put.fail_closed,
          "IPAR-P1-10 DDL security snapshot should cache");

  server::IparSecurityPolicyLookup cross = lookup;
  cross.cache_key = ddl_put.record.cache_key;
  cross.epoch = Epoch(30);
  cross.operation_class = server::IparDmlDdlOperationClass::kDdl;
  cross.operation_id = "ddl.alter_table";
  cross.object_uuid = "wrong-object";
  const auto cross_refusal = service.LookupSecurityPolicySnapshot(cross);
  Require(cross_refusal.fail_closed &&
              cross_refusal.diagnostic_code ==
                  "SB_IPAR_SECURITY_POLICY.CROSS_AUTHORITY",
          "IPAR-P1-10 cross-object cached security authority must refuse");

  auto unsafe_snapshot = Snapshot(server::IparDmlDdlOperationClass::kDml, Epoch(50));
  unsafe_snapshot.authority.cache_is_authorization_authority = true;
  const auto unsafe = service.PutSecurityPolicySnapshot(unsafe_snapshot);
  Require(unsafe.fail_closed &&
              unsafe.diagnostic_code ==
                  "SB_IPAR_SECURITY_POLICY.AUTHORITY_DRIFT",
          "IPAR-P1-10 cache authority drift must fail closed");
}

void ProveCompiledPredicateCache() {
  server::IparSecurityPolicySupportService service;
  server::IparCompiledPredicatePut predicate;
  predicate.predicate_id = "rls-owned-by-principal";
  predicate.predicate_kind = server::IparPredicateKind::kRowLevelSecurity;
  predicate.database_uuid = "db-uuid";
  predicate.object_uuid = "table-dml-uuid";
  predicate.canonical_sblr_predicate = "sblr.predicate.uuid-equals-principal";
  predicate.expression_digest = "sha256:predicate";
  predicate.epoch = Epoch(70);
  const auto put = service.PutCompiledPredicate(predicate);
  Require(put.accepted && !put.fail_closed && !put.plan.plan_digest.empty(),
          "IPAR-P1-16 compiled predicate should cache");

  server::IparCompiledPredicateEval eval;
  eval.cache_key = put.plan.cache_key;
  eval.database_uuid = "db-uuid";
  eval.object_uuid = "table-dml-uuid";
  eval.principal_uuid = "principal-uuid";
  eval.auth_context_uuid = "auth-context-uuid";
  eval.row_fact_digest = "row-owner:principal-uuid";
  eval.epoch = predicate.epoch;
  const auto hit = service.EvaluateCompiledPredicate(eval);
  Require(hit.accepted && hit.cache_hit && !hit.fail_closed,
          "IPAR-P1-16 compiled predicate should evaluate with runtime row facts");
  Require(hit.diagnostic_code == "SB_IPAR_COMPILED_PREDICATE.ALLOWED" ||
              hit.diagnostic_code == "SB_IPAR_COMPILED_PREDICATE.DENIED",
          "IPAR-P1-16 predicate evaluation should produce exact decision diagnostic");

  eval.epoch.policy_generation += 1;
  const auto stale = service.EvaluateCompiledPredicate(eval);
  Require(stale.fail_closed && stale.stale &&
              stale.detail == "policy_epoch_stale",
          "IPAR-P1-16 policy epoch change should stale-refuse predicate");

  predicate.deterministic = false;
  const auto volatile_refusal = service.PutCompiledPredicate(predicate);
  Require(volatile_refusal.fail_closed &&
              volatile_refusal.diagnostic_code ==
                  "SB_IPAR_COMPILED_PREDICATE.VOLATILE_REFUSED",
          "IPAR-P1-16 volatile predicate must not cache");
}

void ProveSlowPathDiagnosticsAndObservability() {
  server::IparSlowPathDiagnostic diagnostic;
  diagnostic.statement_id = "stmt-001";
  diagnostic.operation_class = server::IparDmlDdlOperationClass::kDml;
  diagnostic.chosen_path = "reject_bisection";
  diagnostic.reason_code = "row_policy_runtime_filter";
  diagnostic.validation_stage = "security_policy_recheck";
  diagnostic.required_action = "inspect_sys_ipar_slow_path_reasons";
  const auto slow_path = server::BuildIparSlowPathDiagnostic(diagnostic);
  Require(slow_path.accepted && !slow_path.fail_closed,
          "IPAR-P6-08 slow-path diagnostic should be accepted");
  Require(slow_path.labels.at("required_action") ==
              "inspect_sys_ipar_slow_path_reasons",
          "IPAR-P6-08 slow-path diagnostic should expose required action");
  Require(slow_path.labels.at("authority_scope").find("cache_preflight") !=
              std::string::npos &&
              slow_path.labels.at("finality_authority") == "false",
          "IPAR-P6-08 slow-path diagnostic must expose authority boundary");

  std::vector<server::IparDmlDdlStageObservation> observations;
  observations.push_back({"authorization",
                          "dml.insert",
                          server::IparDmlDdlOperationClass::kDml,
                          120,
                          4,
                          2,
                          64,
                          0,
                          "SB_IPAR_AUTHORIZATION.CACHE_HIT",
                          true,
                          true});
  observations.push_back({"ddl_publish",
                          "ddl.alter_table",
                          server::IparDmlDdlOperationClass::kDdl,
                          300,
                          11,
                          7,
                          1,
                          0,
                          "SB_IPAR_DDL.PUBLISH_PACKET_READY",
                          true,
                          true});
  const auto rows = server::BuildIparDmlDdlObservabilityRows(observations);
  Require(rows.accepted && rows.rows.size() == 2,
          "IPAR-P6-15 DML/DDL observability rows should be source-backed");
  Require(rows.rows[0].source_state == "source_backed" &&
              rows.rows[0].cpu_microseconds == 120 &&
              rows.rows[1].operation_class == "ddl",
          "IPAR-P6-15 rows should expose CPU queue lock and operation class");

  observations[0].authorized_projection = false;
  const auto refused = server::BuildIparDmlDdlObservabilityRows(observations);
  Require(refused.fail_closed &&
              refused.diagnostic_code == "SB_IPAR_OBSERVABILITY.ROW_INVALID",
          "IPAR-P6-15 unauthorized observability projection must refuse");
}

void ProvePolicyResourcePackLoading() {
  server::IparPolicyResourcePackLoadRequest request;
  request.database_uuid = "db-uuid";
  request.current_policy_epoch = 100;
  request.current_resource_epoch = 200;
  request.minimum_compatibility_generation = 3;
  request.create_database_seed = true;
  request.reload = true;
  request.packs = {
      {"memory-cache-default", "memory_cache", "v1", "hash:memory", 3, true, false},
      {"storage-default", "storage", "v1", "hash:storage", 3, true, false},
      {"security-default", "security", "v1", "hash:security", 3, true, false},
      {"optimizer-default", "optimizer", "v1", "hash:optimizer", 3, true, false},
  };
  const auto loaded = server::PlanIparPolicyResourcePackLoad(request);
  Require(loaded.accepted && !loaded.fail_closed,
          "IPAR-P6-16 default policy/resource packs should load");
  Require(loaded.policy_epoch_after == 101 &&
              loaded.resource_epoch_after == 201 &&
              loaded.invalidated_cache_families.size() >= 4,
          "IPAR-P6-16 reload should bump epochs and invalidate affected caches");

  request.packs.pop_back();
  const auto incomplete = server::PlanIparPolicyResourcePackLoad(request);
  Require(incomplete.fail_closed &&
              incomplete.diagnostic_code ==
                  "SB_IPAR_PACK_LOAD.DEFAULT_PACKS_INCOMPLETE",
          "IPAR-P6-16 incomplete default pack set must refuse create database seed");

  request.packs.push_back(
      {"optimizer-default", "optimizer", "v1", "hash:optimizer", 2, true, false});
  request.create_database_seed = false;
  const auto incompatible = server::PlanIparPolicyResourcePackLoad(request);
  Require(incompatible.fail_closed &&
              incompatible.diagnostic_code == "SB_IPAR_PACK_LOAD.VERSION_REFUSED",
          "IPAR-P6-16 incompatible policy/resource pack version must refuse");
}

void ProveStatementPreflight() {
  server::IparStatementPreflightRequest request;
  request.statement_shape_hash = "shape:dml-insert";
  request.operation_id = "dml.insert";
  request.operation_class = server::IparDmlDdlOperationClass::kDml;
  request.resource_budget_bytes = 1024 * 1024;
  request.estimated_bytes = 4096;
  request.epoch = Epoch(300);
  const auto admit = server::PlanIparStatementAdmissionPreflight(request);
  Require(admit.decision == server::IparPreflightDecision::kAdmit &&
              admit.cacheable && !admit.cache_key.empty(),
          "IPAR-P7-15 statement preflight should admit valid DML shape");

  request.transaction_active = false;
  const auto no_txn = server::PlanIparStatementAdmissionPreflight(request);
  Require(no_txn.decision == server::IparPreflightDecision::kRefuse &&
              no_txn.diagnostic_code == "SB_IPAR_PREFLIGHT.TRANSACTION_REQUIRED",
          "IPAR-P7-15 preflight should refuse before expensive work without active MGA transaction");

  request.transaction_active = true;
  request.estimated_bytes = 2 * 1024 * 1024;
  const auto resource_recheck =
      server::PlanIparStatementAdmissionPreflight(request);
  Require(resource_recheck.decision ==
              server::IparPreflightDecision::kExecutorGovernanceRequired &&
              !resource_recheck.cacheable,
          "IPAR-P7-15 resource-sensitive preflight should require executor governance");

  request.authority.finality_authority = true;
  const auto drift = server::PlanIparStatementAdmissionPreflight(request);
  Require(drift.fail_closed &&
              drift.diagnostic_code == "SB_IPAR_PREFLIGHT.AUTHORITY_DRIFT",
          "IPAR-P7-15 preflight must refuse finality authority drift");
}

}  // namespace

int main() {
  ProveSecurityPolicySnapshotCache();
  ProveCompiledPredicateCache();
  ProveSlowPathDiagnosticsAndObservability();
  ProvePolicyResourcePackLoading();
  ProveStatementPreflight();
  std::cout << "ipar_security_policy_support_gate=passed\n";
  return EXIT_SUCCESS;
}
