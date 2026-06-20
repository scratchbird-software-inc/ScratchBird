// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_protocol_support.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using scratchbird::server::IparBatchWindow;
using scratchbird::server::IparCacheStatus;
using scratchbird::server::IparFailureClassificationResult;
using scratchbird::server::IparFrameCryptoLease;
using scratchbird::server::IparProtocolOperationClass;
using scratchbird::server::IparProtocolSupportMetrics;
using scratchbird::server::IparResultBufferLease;
using scratchbird::server::IparServerProtocolSupport;
using scratchbird::server::IparSupportScopeForSession;
using scratchbird::server::IparSupportSessionScope;
using scratchbird::server::IparUuidDependency;
using scratchbird::server::ServerSessionRecord;

void Require(bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
  }
}

void RequireAccepted(const IparCacheStatus& status,
                     const std::string& label) {
  Require(status.accepted, label + " accepted detail=" + status.detail);
}

void RequireRejectedDetail(const IparCacheStatus& status,
                           const std::string& detail,
                           const std::string& label) {
  Require(!status.accepted, label + " rejected");
  Require(status.detail == detail,
          label + " detail expected=" + detail + " actual=" + status.detail);
}

void RequireLeaseRejectedDetail(const IparFrameCryptoLease& lease,
                                const std::string& detail,
                                const std::string& label) {
  Require(!lease.accepted, label + " rejected");
  Require(lease.detail == detail,
          label + " detail expected=" + detail + " actual=" + lease.detail);
}

void RequireBufferRejectedDetail(const IparResultBufferLease& lease,
                                 const std::string& detail,
                                 const std::string& label) {
  Require(!lease.accepted, label + " rejected");
  Require(lease.detail == detail,
          label + " detail expected=" + detail + " actual=" + lease.detail);
}

void RequireFailureRejectedDetail(const IparFailureClassificationResult& result,
                                  const std::string& detail,
                                  const std::string& label) {
  Require(!result.accepted, label + " rejected");
  Require(result.detail == detail,
          label + " detail expected=" + detail + " actual=" + result.detail);
}

std::array<std::uint8_t, 16> UuidBytes(std::uint8_t seed) {
  return {0x01, 0x9f, 0x40, seed, 0x00, 0x00, 0x70, 0x00,
          0x80, 0x00, 0x00, 0x00, 0x00, seed, seed, seed};
}

std::string UuidText(unsigned int suffix) {
  std::ostringstream out;
  out << "019f4000-0000-7000-8000-" << std::setw(12)
      << std::setfill('0') << suffix;
  return out.str();
}

ServerSessionRecord MakeSession(std::uint8_t seed) {
  ServerSessionRecord session;
  session.session_uuid = UuidBytes(seed);
  session.auth_context_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 10));
  session.principal_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 20));
  session.effective_user_uuid = UuidBytes(static_cast<std::uint8_t>(seed + 30));
  session.database_uuid = UuidText(900 + seed);
  session.catalog_generation = 101;
  session.security_epoch = 201;
  session.descriptor_epoch = 301;
  session.grant_epoch = 401;
  session.policy_generation = 501;
  session.capability_policy_generation = 601;
  session.cache_invalidation_epoch = 701;
  session.name_resolution_epoch = 801;
  session.resource_epoch = 901;
  session.role_set_hash = "sha256:roles-a";
  session.group_set_hash = "sha256:groups-a";
  session.search_path_hash = "sha256:search-path-a";
  return session;
}

IparUuidDependency DependencyFor(const IparSupportSessionScope& scope,
                                 unsigned int suffix) {
  IparUuidDependency dependency;
  dependency.dependency_kind = "table";
  dependency.logical_name = "R" + std::to_string(suffix);
  dependency.object_uuid = UuidText(100 + suffix);
  dependency.descriptor_uuid = UuidText(200 + suffix);
  dependency.descriptor_hash = "sha256:descriptor-" + std::to_string(suffix);
  dependency.catalog_generation = scope.epoch.catalog_generation;
  dependency.descriptor_epoch = scope.epoch.descriptor_epoch;
  dependency.name_resolution_epoch = scope.epoch.name_resolution_epoch;
  return dependency;
}

scratchbird::server::IparPreparedTemplatePut MakePreparedPut(
    const IparSupportSessionScope& scope) {
  scratchbird::server::IparPreparedTemplatePut request;
  request.scope = scope;
  request.operation_id = "dml.insert_rows";
  request.operation_family = "sblr.dml.insert.v3";
  request.result_descriptor_hash = "sha256:insert-result-descriptor";
  request.dependencies.push_back(DependencyFor(scope, 1));
  request.canonical_sblr_envelope =
      "envelope=SBLRExecutionEnvelope.v3\n"
      "operation_id=dml.insert_rows\n"
      "operation_family=sblr.dml.insert.v3\n"
      "target_object_uuid=" +
      request.dependencies.front().object_uuid +
      "\nparser_resolved_names_to_uuids=true\n";
  return request;
}

scratchbird::server::IparPreparedTemplateLookup MakePreparedLookup(
    const IparSupportSessionScope& scope,
    const std::string& cache_key,
    const scratchbird::server::IparPreparedTemplatePut& put) {
  scratchbird::server::IparPreparedTemplateLookup lookup;
  lookup.scope = scope;
  lookup.cache_key = cache_key;
  lookup.operation_id = put.operation_id;
  lookup.operation_family = put.operation_family;
  lookup.canonical_sblr_envelope = put.canonical_sblr_envelope;
  lookup.result_descriptor_hash = put.result_descriptor_hash;
  lookup.dependencies = put.dependencies;
  return lookup;
}

scratchbird::server::IparResolvedDescriptorPut MakeDescriptorPut(
    const IparSupportSessionScope& scope) {
  scratchbird::server::IparResolvedDescriptorPut request;
  request.scope = scope;
  request.resolved_name = "R1";
  request.object_uuid = UuidText(101);
  request.object_kind = "relation";
  request.descriptor_uuid = UuidText(201);
  request.descriptor_hash = "sha256:descriptor-1";
  request.operation_id = "dml.insert_rows";
  return request;
}

void ProvePreparedTemplateCache() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  const auto put = MakePreparedPut(scope);
  const IparCacheStatus stored = support.StorePreparedTemplate(put);
  RequireAccepted(stored, "prepared template stored");
  Require(!stored.cache_key.empty(), "prepared template cache key present");

  auto lookup = MakePreparedLookup(scope, stored.cache_key, put);
  IparCacheStatus hit = support.LookupPreparedTemplate(lookup);
  RequireAccepted(hit, "prepared template lookup");
  Require(hit.cache_hit, "prepared template cache hit");

  auto cross_session = lookup;
  cross_session.scope = IparSupportScopeForSession(MakeSession(2));
  RequireRejectedDetail(support.LookupPreparedTemplate(cross_session),
                        "ipar_cache_cross_session",
                        "prepared template cross-session invalidation");

  auto stale_catalog = lookup;
  ++stale_catalog.scope.epoch.catalog_generation;
  RequireRejectedDetail(support.LookupPreparedTemplate(stale_catalog),
                        "ipar_cache_epoch_stale",
                        "prepared template catalog invalidation");

  auto stale_role = lookup;
  stale_role.scope.epoch.role_set_hash = "sha256:roles-b";
  RequireRejectedDetail(support.LookupPreparedTemplate(stale_role),
                        "ipar_cache_authorization_hash_stale",
                        "prepared template role invalidation");

  auto stale_capability = lookup;
  ++stale_capability.scope.epoch.capability_policy_generation;
  RequireRejectedDetail(support.LookupPreparedTemplate(stale_capability),
                        "ipar_cache_capability_stale",
                        "prepared template capability invalidation");

  auto sql_text = put;
  sql_text.parser_sql_text_present = true;
  RequireRejectedDetail(support.StorePreparedTemplate(sql_text),
                        "ipar_template_sql_text_forbidden",
                        "prepared template SQL text boundary");

  auto sql_payload = put;
  sql_payload.canonical_sblr_envelope += "sql_text=select * from R1\n";
  RequireRejectedDetail(support.StorePreparedTemplate(sql_payload),
                        "ipar_template_sql_text_forbidden",
                        "prepared template embedded SQL boundary");

  auto auth_authority = put;
  auth_authority.authority_flags.client_or_parser_authorization_authority =
      true;
  RequireRejectedDetail(support.StorePreparedTemplate(auth_authority),
                        "ipar_cache_authorization_authority_forbidden",
                        "prepared template authorization authority boundary");

  auto finality_authority = put;
  finality_authority.authority_flags.transaction_finality_authority = true;
  RequireRejectedDetail(support.StorePreparedTemplate(finality_authority),
                        "ipar_cache_finality_authority_forbidden",
                        "prepared template finality authority boundary");

  const auto& record = support.prepared_templates().at(stored.cache_key);
  Require(!record.authority_flags.client_or_parser_authorization_authority,
          "stored prepared template is not authorization authority");
  Require(!record.authority_flags.transaction_finality_authority,
          "stored prepared template is not finality authority");
  Require(!record.authority_flags.visibility_authority,
          "stored prepared template is not visibility authority");
  Require(!record.authority_flags.grants_authority,
          "stored prepared template is not grants authority");

  const IparProtocolSupportMetrics& metrics = support.metrics();
  Require(metrics.prepared_template_hits == 1,
          "prepared template hit metric");
  Require(metrics.stale_rejections >= 3,
          "prepared template stale rejection metric");
  Require(metrics.authority_rejections >= 4,
          "prepared template authority rejection metric");
}

void ProveResolvedDescriptorCache() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  auto put = MakeDescriptorPut(scope);
  IparCacheStatus stored = support.StoreResolvedDescriptor(put);
  RequireAccepted(stored, "descriptor stored");
  put.cache_key = stored.cache_key;

  IparCacheStatus hit = support.LookupResolvedDescriptor(put);
  RequireAccepted(hit, "descriptor lookup");
  Require(hit.cache_hit, "descriptor cache hit");

  auto stale_name = put;
  ++stale_name.scope.epoch.name_resolution_epoch;
  RequireRejectedDetail(support.LookupResolvedDescriptor(stale_name),
                        "ipar_cache_epoch_stale",
                        "descriptor name epoch invalidation");

  auto stale_descriptor = put;
  ++stale_descriptor.scope.epoch.descriptor_epoch;
  RequireRejectedDetail(support.LookupResolvedDescriptor(stale_descriptor),
                        "ipar_cache_epoch_stale",
                        "descriptor descriptor epoch invalidation");

  auto stale_capability = put;
  ++stale_capability.scope.epoch.capability_policy_generation;
  RequireRejectedDetail(support.LookupResolvedDescriptor(stale_capability),
                        "ipar_cache_capability_stale",
                        "descriptor capability invalidation");

  auto invalid_uuid = MakeDescriptorPut(scope);
  invalid_uuid.object_uuid = "not-a-uuid";
  RequireRejectedDetail(support.StoreResolvedDescriptor(invalid_uuid),
                        "ipar_descriptor_uuid_shape_required",
                        "descriptor UUID shape gate");

  auto grant_authority = MakeDescriptorPut(scope);
  grant_authority.authority_flags.grants_authority = true;
  RequireRejectedDetail(support.StoreResolvedDescriptor(grant_authority),
                        "ipar_cache_grant_forbidden",
                        "descriptor grant authority boundary");

  const auto& record = support.descriptors().at(stored.cache_key);
  Require(!record.authority_flags.client_or_parser_authorization_authority,
          "stored descriptor is not authorization authority");
  Require(!record.authority_flags.grants_authority,
          "stored descriptor is not grant authority");
}

void ProveBatchingAndDmlDdl() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  scratchbird::server::IparProtocolBatchRequest request;
  request.scope = scope;
  request.operation_class = IparProtocolOperationClass::kDml;
  request.operation_id = "dml.insert_rows";
  request.requested_rows = 125;
  request.estimated_payload_bytes = 12500;
  request.max_batch_rows = 50;
  request.max_frame_bytes = 4096;
  request.driver_capability_generation =
      scope.epoch.capability_policy_generation;
  request.server_capability_generation =
      scope.epoch.capability_policy_generation;
  request.driver_supports_batching = true;
  request.contains_dml = true;

  const auto plan = support.PlanProtocolBatch(request);
  Require(plan.accepted, "DML batch plan accepted");
  Require(plan.driver_batching_enabled, "DML driver batching enabled");
  Require(plan.server_revalidation_required,
          "DML batch requires server revalidation");
  Require(!plan.capability_cache_key.empty(),
          "DML batch capability cache key present");
  Require(plan.windows.size() > 1, "DML batch has multiple windows");
  std::uint64_t planned_rows = 0;
  for (const IparBatchWindow& window : plan.windows) {
    planned_rows += window.row_count;
    Require(window.estimated_bytes <= request.max_frame_bytes,
            "DML batch window honors frame size");
  }
  Require(planned_rows == request.requested_rows,
          "DML batch windows cover requested rows");

  auto ddl = request;
  ddl.operation_class = IparProtocolOperationClass::kDdl;
  ddl.operation_id = "ddl.create_relation";
  ddl.requested_rows = 1;
  ddl.estimated_payload_bytes = 1024;
  ddl.contains_dml = false;
  ddl.contains_ddl = true;
  const auto ddl_plan = support.PlanProtocolBatch(ddl);
  Require(ddl_plan.accepted, "DDL protocol plan accepted");
  Require(!ddl_plan.driver_batching_enabled, "DDL driver batching disabled");
  Require(ddl_plan.windows.size() == 1, "DDL uses one protocol window");
  Require(ddl_plan.detail == "ipar_ddl_single_operation",
          "DDL plan classified");

  auto mixed = request;
  mixed.contains_ddl = true;
  const auto mixed_plan = support.PlanProtocolBatch(mixed);
  Require(!mixed_plan.accepted, "mixed DML DDL refused");
  Require(mixed_plan.dml_ddl_mixed_refused, "mixed DML DDL flag");
  Require(mixed_plan.detail == "ipar_batch_dml_ddl_mixed_refused",
          "mixed DML DDL detail");

  auto stale_capability = request;
  ++stale_capability.driver_capability_generation;
  const auto stale_plan = support.PlanProtocolBatch(stale_capability);
  Require(!stale_plan.accepted, "stale batch capability refused");
  Require(stale_plan.detail == "ipar_batch_capability_epoch_stale",
          "stale batch capability detail");

  auto finality = request;
  finality.parser_or_driver_finality_authority = true;
  const auto finality_plan = support.PlanProtocolBatch(finality);
  Require(!finality_plan.accepted, "batch finality authority refused");
  Require(finality_plan.detail == "ipar_batch_finality_authority_forbidden",
          "batch finality authority detail");
}

void ProveFrameCryptoReuse() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  scratchbird::server::IparFrameCryptoRequest request;
  request.scope = scope;
  request.frame_class = "execute";
  request.payload_capacity = 4096;
  request.crypto_epoch = 11;
  request.handshake_generation = 12;
  request.tls_policy_hash = "sha256:tls-policy-a";
  request.key_lineage_id = "key-lineage-a";

  const IparFrameCryptoLease first =
      support.AcquireFrameCryptoLease(request);
  Require(first.accepted, "frame crypto first lease accepted");
  Require(!first.frame_reused, "frame crypto first lease not reused");
  Require(!first.crypto_context_reused,
          "frame crypto first lease has new crypto context");
  Require(!first.stores_plaintext_key_material,
          "frame crypto lease stores no plaintext key material");
  RequireAccepted(support.ReleaseFrameCryptoLease(scope, first.reuse_key),
                  "frame crypto release");

  auto reuse = request;
  reuse.requested_reuse_key = first.reuse_key;
  const IparFrameCryptoLease second =
      support.AcquireFrameCryptoLease(reuse);
  Require(second.accepted, "frame crypto reused lease accepted");
  Require(second.frame_reused, "frame pool reused");
  Require(second.crypto_context_reused, "crypto context reused");
  Require(!second.stores_plaintext_key_material,
          "reused frame crypto lease stores no plaintext");

  auto stale_capability = reuse;
  ++stale_capability.scope.epoch.capability_policy_generation;
  RequireLeaseRejectedDetail(
      support.AcquireFrameCryptoLease(stale_capability),
      "ipar_cache_capability_stale",
      "frame crypto capability invalidation");

  auto plaintext = request;
  plaintext.contains_plaintext_database_key_material = true;
  RequireLeaseRejectedDetail(
      support.AcquireFrameCryptoLease(plaintext),
      "ipar_crypto_plaintext_key_material_forbidden",
      "frame crypto plaintext key boundary");

  auto authority = request;
  authority.parser_or_driver_authority = true;
  RequireLeaseRejectedDetail(
      support.AcquireFrameCryptoLease(authority),
      "ipar_frame_crypto_authority_forbidden",
      "frame crypto parser authority boundary");

  Require(support.metrics().frame_reuses == 1, "frame reuse metric");
  Require(support.metrics().crypto_reuses == 1, "crypto reuse metric");
}

void ProveResultBufferReuse() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  scratchbird::server::IparResultBufferRequest request;
  request.scope = scope;
  request.operation_id = "dml.update_rows";
  request.result_shape_hash = "sha256:returning-shape";
  request.target_object_uuid = UuidText(101);
  request.minimum_capacity = 8192;
  request.returning_path = true;

  const IparResultBufferLease first = support.AcquireResultBuffer(request);
  Require(first.accepted, "result buffer first lease accepted");
  Require(!first.cache_hit, "result buffer first lease miss");
  Require(first.returning_path, "result buffer returning path flagged");
  Require(!first.finality_authority,
          "result buffer lease is not finality authority");
  RequireAccepted(support.ReleaseResultBuffer(scope, first.reuse_key),
                  "result buffer release");

  auto reuse = request;
  reuse.requested_reuse_key = first.reuse_key;
  const IparResultBufferLease second = support.AcquireResultBuffer(reuse);
  Require(second.accepted, "result buffer reused lease accepted");
  Require(second.cache_hit, "result buffer cache hit");
  Require(second.returning_path, "result buffer reused returning path flagged");
  Require(!second.finality_authority,
          "reused result buffer is not finality authority");

  auto stale_descriptor = reuse;
  ++stale_descriptor.scope.epoch.descriptor_epoch;
  RequireBufferRejectedDetail(
      support.AcquireResultBuffer(stale_descriptor),
      "ipar_cache_epoch_stale",
      "result buffer descriptor epoch invalidation");

  auto finality = request;
  finality.parser_or_driver_finality_authority = true;
  RequireBufferRejectedDetail(
      support.AcquireResultBuffer(finality),
      "ipar_result_buffer_finality_authority_forbidden",
      "result buffer finality authority boundary");

  Require(support.metrics().result_buffer_reuses == 1,
          "result buffer reuse metric");
}

void ProveHotFailureClassification() {
  IparServerProtocolSupport support;
  const IparSupportSessionScope scope =
      IparSupportScopeForSession(MakeSession(1));
  scratchbird::server::IparFailureClassificationRequest request;
  request.scope = scope;
  request.diagnostic_code = "PARSER_SERVER_IPC.AUTHORITY_CACHE_STALE";
  request.diagnostic_detail = "authority_cache_epoch_stale";
  request.operation_id = "dml.insert_rows";
  request.statement_shape_hash = "sha256:statement-shape";
  request.engine_finality_known = true;

  IparFailureClassificationResult classified =
      support.ClassifyFailure(request);
  Require(classified.accepted, "stale cache failure classified");
  Require(classified.failure_class == "stale_cache",
          "stale cache failure class");
  Require(classified.retryable, "stale cache failure retryable");
  Require(!classified.requires_mga_inventory_check,
          "stale cache failure does not claim MGA finality");
  Require(!classified.finality_authority,
          "failure cache result is not finality authority");

  request.requested_cache_key = classified.cache_key;
  IparFailureClassificationResult lookup =
      support.LookupFailureClassification(request);
  Require(lookup.accepted, "failure classification lookup accepted");
  Require(lookup.cache_hit, "failure classification cache hit");

  auto stale_role = request;
  stale_role.scope.epoch.role_set_hash = "sha256:roles-b";
  RequireFailureRejectedDetail(
      support.LookupFailureClassification(stale_role),
      "ipar_cache_authorization_hash_stale",
      "failure classification role invalidation");

  auto stale_capability = request;
  ++stale_capability.scope.epoch.capability_policy_generation;
  RequireFailureRejectedDetail(
      support.LookupFailureClassification(stale_capability),
      "ipar_cache_capability_stale",
      "failure classification capability invalidation");

  scratchbird::server::IparFailureClassificationRequest unknown;
  unknown.scope = scope;
  unknown.diagnostic_code = "PARSER_SERVER_IPC.ENGINE_DISPATCH_FAILED";
  unknown.diagnostic_detail = "engine outcome unknown";
  unknown.operation_id = "dml.delete_rows";
  unknown.statement_shape_hash = "sha256:delete-shape";
  unknown.engine_finality_known = false;
  const IparFailureClassificationResult unknown_result =
      support.ClassifyFailure(unknown);
  Require(unknown_result.accepted, "unknown engine outcome classified");
  Require(unknown_result.failure_class ==
              "engine_outcome_requires_mga_inventory",
          "unknown outcome requires MGA inventory class");
  Require(unknown_result.requires_mga_inventory_check,
          "unknown outcome requires MGA inventory check");
  Require(unknown_result.retryable, "unknown outcome retryable");
  Require(!unknown_result.ends_transaction,
          "failure classification never ends transaction");
  Require(!unknown_result.finality_authority,
          "unknown outcome cache is not finality authority");

  auto finality = unknown;
  finality.parser_or_driver_finality_authority = true;
  RequireFailureRejectedDetail(
      support.ClassifyFailure(finality),
      "ipar_failure_finality_authority_forbidden",
      "failure classification finality authority boundary");

  Require(support.metrics().failure_cache_hits == 1,
          "failure classification hit metric");
}

}  // namespace

int main() {
  ProvePreparedTemplateCache();
  ProveResolvedDescriptorCache();
  ProveBatchingAndDmlDdl();
  ProveFrameCryptoReuse();
  ProveResultBufferReuse();
  ProveHotFailureClassification();
  std::cout << "ipar_server_protocol_cache_support_gate=passed\n";
  return 0;
}
