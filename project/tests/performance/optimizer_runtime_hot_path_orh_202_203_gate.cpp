// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"
#include "observability/performance_metric_event.hpp"
#include "prepared_execution_template.hpp"
#include "runtime_consumption_evidence.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_prepared_template.hpp"
#include "vectorized_result_batch.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace sblr = scratchbird::engine::sblr;
namespace wire = scratchbird::wire;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ORH-202/203 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::RuntimeOptimizedPathEvidence RuntimeEvidence(std::string route_kind,
                                                  std::string selected_path) {
  auto evidence = opt::MakeSelectionOnlyRuntimeEvidence(
      std::move(selected_path),
      std::move(route_kind),
      "SB_ORH_202_203.RUNTIME_SELECTED",
      "selected optimized route has not been consumed");
  evidence.catalog_epoch = 17;
  evidence.security_epoch = 23;
  evidence.redaction_epoch = 29;
  evidence.provider_generation = 31;
  evidence.transaction_snapshot_class = "mga_statement_snapshot";
  evidence.result_contract_hash = "hash:orh-202-203-result-contract";
  return evidence;
}

opt::RuntimeOptimizedPathEvidence ConsumedRuntimeEvidence(
    std::string route_kind,
    std::string selected_path) {
  auto evidence = opt::MarkRuntimeEvidenceConsumed(
      RuntimeEvidence(std::move(route_kind), std::move(selected_path)),
      "server.wire.engine.prepared_low_overhead_route");
  evidence.diagnostic_code = "SB_ORH_202_203.RUNTIME_CONSUMED";
  return evidence;
}

api::EngineUuid Uuid(const std::string& value) {
  api::EngineUuid uuid;
  uuid.canonical = value;
  return uuid;
}

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(uuid);
  descriptor.descriptor_kind = "executor.scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor = "type=" + type + ";uuid=" + uuid;
  return descriptor;
}

api::EngineColumnDefinition Column(const std::string& uuid,
                                   const std::string& type,
                                   std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid = Uuid(uuid);
  column.descriptor = Descriptor("desc:" + uuid, type);
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
}

api::EngineRequestContext PreparedRouteContext(const std::string& family) {
  api::EngineRequestContext context;
  context.database_uuid = Uuid("db.orh202." + family);
  context.principal_uuid = Uuid("principal.orh202");
  context.current_role_uuid = Uuid("role.reader");
  context.session_uuid = Uuid("session.orh202." + family);
  context.transaction_uuid = Uuid("txn.orh202." + family);
  context.local_transaction_id = 88;
  context.snapshot_visible_through_local_transaction_id = 55;
  context.transaction_isolation_level = "snapshot";
  context.security_context_present = true;
  context.catalog_generation_id = 20;
  context.security_epoch = 21;
  context.resource_epoch = 22;
  context.name_resolution_epoch = 23;
  return context;
}

api::EngineApiRequest PreparedRouteRequest(
    const api::EngineRequestContext& context,
    const std::string& family) {
  api::EngineApiRequest request;
  request.context = context;
  request.operation_id = "orh202." + family;
  request.target_database.uuid = context.database_uuid;
  request.target_database.object_kind = "database";
  request.target_schema.uuid = Uuid("schema.public");
  request.target_schema.object_kind = "schema";
  request.target_object.uuid = Uuid("rel.orh202." + family);
  request.target_object.object_kind = "relation";
  request.related_objects = {{request.target_object.uuid, "relation"}};
  request.columns = {
      Column("col." + family + ".id", "int64", 0),
      Column("col." + family + ".value", "text", 1),
  };
  request.descriptors = {request.columns[0].descriptor,
                         request.columns[1].descriptor};
  request.bound_object_identity.object_uuid = request.target_object.uuid;
  request.bound_object_identity.resolved_schema_uuid =
      request.target_schema.uuid;
  request.bound_object_identity.catalog_generation_id =
      context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.predicate.predicate_kind = "scalar_eq";
  request.predicate.canonical_predicate_envelope =
      "sblr.predicate.uuid_bound.v1";
  return request;
}

sblr::SblrOperationEnvelope PreparedRouteEnvelope(const std::string& family) {
  auto envelope = sblr::MakeSblrEnvelope("orh202." + family,
                                         "SBLR_ORH202_" + family,
                                         "trace.orh202." + family);
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.result_shape = "engine.result.orh202." + family + ".v1";
  envelope.operands.push_back({"predicate_slot", "predicate:scalar_eq",
                               "col." + family + ".id"});
  envelope.operands.push_back({"parameter_slot", "param:0",
                               "col." + family + ".id"});
  return envelope;
}

opt::FixedRouteOverheadEvidence FixedRouteEvidenceFromRuntimePreparedFamily(
    const std::string& family) {
  exec::PreparedTemplateCache cache;
  const auto context = PreparedRouteContext(family);
  const auto request = PreparedRouteRequest(context, family);
  const auto envelope = PreparedRouteEnvelope(family);
  const auto build = sblr::BuildPreparedTemplateFromSblr(envelope, context,
                                                         request);
  Require(build.ok, family + " SBLR prepared template build failed: " +
                        build.diagnostic_code);
  Require(Contains(build.evidence, "parser_sql_text_authority=false"),
          family + " SBLR builder lost parser authority evidence");

  const auto first_prepare = cache.Prepare(build.admission);
  Require(first_prepare.ok,
          family + " first prepared template prepare failed: " +
              first_prepare.diagnostic_code);
  const auto warmed_prepare = cache.Prepare(build.admission);
  Require(warmed_prepare.ok && warmed_prepare.reused_existing_template,
          family + " warmed prepared template was not reused");
  const auto bound = cache.LookupAndBind(warmed_prepare.prepared_template->key,
                                         build.bind_context);
  Require(bound.ok,
          family + " warmed prepared template bind failed: " +
              bound.diagnostic_code);

  exec::PreparedRouteOverheadObservation observation;
  observation.route_kind = "embedded";
  observation.statement_family = family;
  observation.selected_path = "prepared_lowered_sblr_reuse_v1";
  observation.benchmark_clean_candidate = true;
  observation.prepare_result = &warmed_prepare;
  observation.bind_result = &bound;
  observation.lowered_sblr_reused =
      Contains(build.evidence, "sblr_prepared_template_source=operation_envelope");
  observation.text_rendering_suppressed = true;
  observation.route_latency_budget_us = 75;
  observation.route_latency_observed_us = 31;
  observation.runtime_evidence =
      ConsumedRuntimeEvidence(observation.route_kind,
                              observation.selected_path);
  observation.diagnostic_code =
      "SB_ORH_FIXED_ROUTE_OVERHEAD.PREPARED_ROUTE_OBSERVED";
  return exec::BuildFixedRouteOverheadEvidenceFromPreparedRoute(observation);
}

void FixedRouteOverheadCleanSelectInsertAggregatePass() {
  for (const std::string family : {"select", "insert", "aggregate"}) {
    const auto validation = opt::ValidateFixedRouteOverheadEvidence(
        FixedRouteEvidenceFromRuntimePreparedFamily(family));
    Require(validation.ok, family + " fixed-route evidence rejected");
    Require(validation.benchmark_clean,
            family + " fixed-route evidence not benchmark-clean");
    Require(validation.diagnostic_code ==
                "SB_ORH_FIXED_ROUTE_OVERHEAD.BENCHMARK_CLEAN",
            family + " fixed-route diagnostic mismatch");
  }
}

void RepeatedFrontendWorkFailsClosed() {
  auto evidence = FixedRouteEvidenceFromRuntimePreparedFamily("select");
  evidence.repeated_parse_count = 1;
  evidence.fallback_reason = "parser repeated during warmed prepared route";
  evidence.diagnostic_code = "SB_ORH_FIXED_ROUTE_OVERHEAD.REPEATED_PARSE";
  const auto validation =
      opt::ValidateFixedRouteOverheadEvidence(evidence);
  Require(validation.ok, "repeated parse evidence should be coherent");
  Require(!validation.benchmark_clean,
          "repeated parse route was marked benchmark-clean");
  Require(validation.exact_fallback, "repeated parse did not fail closed");
  Require(Contains(validation.diagnostics,
                   "SB_ORH_FIXED_ROUTE_OVERHEAD.REPEATED_FRONTEND_OR_RENDER_OVERHEAD"),
          "repeated frontend overhead diagnostic missing");
}

void IndexDependentRouteDoesNotClaimClosureWithoutProof() {
  auto evidence = FixedRouteEvidenceFromRuntimePreparedFamily("select");
  evidence.index_dependent = true;
  evidence.index_correctness_proven = false;
  evidence.fallback_reason =
      "index-dependent point lookup waits for index correctness proof";
  evidence.diagnostic_code = "SB_ORH_FIXED_ROUTE_OVERHEAD.INDEX_UNPROVEN";
  const auto validation =
      opt::ValidateFixedRouteOverheadEvidence(evidence);
  Require(validation.ok, "index fallback evidence should be coherent");
  Require(!validation.benchmark_clean,
          "index-dependent route closed without index correctness proof");
  Require(validation.exact_fallback,
          "index-dependent route did not record exact fallback");
  Require(Contains(validation.diagnostics,
                   "SB_ORH_FIXED_ROUTE_OVERHEAD.INDEX_CORRECTNESS_UNPROVEN"),
          "index correctness fallback diagnostic missing");
}

void ParserOrCacheAuthorityDriftIsRejected() {
  auto evidence = FixedRouteEvidenceFromRuntimePreparedFamily("insert");
  evidence.parser_or_cache_owns_transaction_finality = true;
  const auto validation =
      opt::ValidateFixedRouteOverheadEvidence(evidence);
  Require(!validation.ok,
          "parser/cache transaction finality authority drift was accepted");
  Require(validation.diagnostic_code ==
              "SB_ORH_FIXED_ROUTE_OVERHEAD.INVALID_CONTRACT",
          "authority drift diagnostic mismatch");
}

exec::VectorizedResultBatch BinaryFrameBatch() {
  exec::VectorizedResultBatchBuilder builder(3);
  builder.AddColumn(exec::MakeFixedWidthResultBatchColumn(
      "id",
      3,
      8,
      {1, 0, 0, 0, 0, 0, 0, 0,
       2, 0, 0, 0, 0, 0, 0, 0,
       3, 0, 0, 0, 0, 0, 0, 0},
      exec::MakeResultBatchValidityBitmap(3)));
  const auto finalized = builder.Finalize();
  Require(finalized.ok(), "vectorized batch fixture did not finalize");
  return finalized.batch;
}

opt::BenchmarkResultFastPathEvidence BinaryResultFastPathFromRuntimeFrame(
    bool disabled = false,
    bool consumed_runtime = true) {
  const auto frame = wire::BuildDirectBinaryResultFrame(BinaryFrameBatch());
  Require(frame.ok(), "direct binary result frame fixture did not build");

  wire::BinaryResultFastPathObservation observation;
  observation.route_kind = "ipc";
  observation.statement_family = "select";
  observation.benchmark_clean_candidate = true;
  observation.frame_result = &frame;
  observation.instrumentation_policy =
      api::InstrumentationOverheadPolicyForMode(
          api::InstrumentationOverheadMode::kBenchmarkClean);
  observation.equivalent_result_materialization = true;
  observation.support_evidence_available_outside_timed_path = true;
  observation.disabled_or_fallback = disabled;
  observation.disabled_reason = disabled ? "client requested text rows" : "";
  observation.runtime_evidence =
      consumed_runtime
          ? ConsumedRuntimeEvidence(observation.route_kind,
                                    "binary_result_fast_path_v1")
          : RuntimeEvidence(observation.route_kind,
                            "binary_result_fast_path_v1");
  observation.result_contract_hash = "hash:binary-result-contract";
  observation.diagnostic_code =
      "SB_ORH_BINARY_RESULT_FAST_PATH.WIRE_ROUTE_OBSERVED";
  return wire::BuildBenchmarkResultFastPathEvidenceFromWireResult(observation);
}

void BinaryResultFastPathBuildsFrameAndPassesCleanPolicy() {
  const auto benchmark_policy = api::InstrumentationOverheadPolicyForMode(
      api::InstrumentationOverheadMode::kBenchmarkClean);
  const auto support_policy = api::InstrumentationOverheadPolicyForMode(
      api::InstrumentationOverheadMode::kSupportBundle);
  Require(benchmark_policy.benchmark_clean_eligible,
          "benchmark-clean policy is not eligible");
  Require(!benchmark_policy.hot_path_string_formatting_enabled,
          "benchmark-clean policy enabled hot-path string formatting");
  Require(!benchmark_policy.support_bundle_summary_enabled,
          "benchmark-clean policy enabled support bundle summary");
  Require(support_policy.support_bundle_summary_enabled,
          "support-bundle policy does not retain outside-timed-path evidence");

  const auto frame = wire::BuildDirectBinaryResultFrame(BinaryFrameBatch());
  Require(frame.ok(), "direct binary result frame did not build");
  Require(frame.frame.descriptor.version == wire::kDirectBinaryResultFrameVersion,
          "direct binary result frame version mismatch");
  Require(frame.frame.descriptor.row_count == 3,
          "direct binary result frame row count mismatch");
  Require(Contains(frame.evidence, "direct_binary_frame.finality_authority=false"),
          "binary frame lost no-finality-authority evidence");

  const auto validation = opt::ValidateBenchmarkResultFastPathEvidence(
      BinaryResultFastPathFromRuntimeFrame());
  Require(validation.ok, "binary result fast-path evidence rejected");
  Require(validation.benchmark_clean,
          "binary result fast-path evidence was not benchmark-clean");
  Require(validation.diagnostic_code ==
              "SB_ORH_BINARY_RESULT_FAST_PATH.BENCHMARK_CLEAN",
          "binary result fast-path diagnostic mismatch");
}

void BinaryFastPathDisabledRecordsExactFallback() {
  auto evidence = BinaryResultFastPathFromRuntimeFrame(true);
  evidence.diagnostic_code = "SB_ORH_BINARY_RESULT_FAST_PATH.DISABLED";
  const auto validation =
      opt::ValidateBenchmarkResultFastPathEvidence(evidence);
  Require(validation.ok, "disabled binary fast-path evidence rejected");
  Require(!validation.benchmark_clean,
          "disabled binary path was marked benchmark-clean");
  Require(validation.exact_fallback,
          "disabled binary path did not record exact fallback");
  Require(Contains(validation.diagnostics,
                   "SB_ORH_BINARY_RESULT_FAST_PATH.DISABLED"),
          "disabled binary fallback diagnostic missing");
}

void BinaryFastPathRequiresRuntimeConsumptionAndDiagnostics() {
  auto evidence = BinaryResultFastPathFromRuntimeFrame(false, false);
  evidence.disabled_reason =
      "binary route selected but runtime consumption evidence missing";
  evidence.diagnostic_code =
      "SB_ORH_BINARY_RESULT_FAST_PATH.RUNTIME_MISSING";
  const auto validation =
      opt::ValidateBenchmarkResultFastPathEvidence(evidence);
  Require(validation.ok,
          "runtime-missing binary fallback evidence should be coherent");
  Require(!validation.benchmark_clean,
          "binary route closed without runtime consumption evidence");
  Require(validation.exact_fallback,
          "missing runtime consumption did not fail closed");
  Require(Contains(validation.diagnostics,
                   "SB_ORH_BINARY_RESULT_FAST_PATH.RUNTIME_CONSUMPTION_MISSING"),
          "runtime consumption diagnostic missing");
}

}  // namespace

int main() {
  FixedRouteOverheadCleanSelectInsertAggregatePass();
  RepeatedFrontendWorkFailsClosed();
  IndexDependentRouteDoesNotClaimClosureWithoutProof();
  ParserOrCacheAuthorityDriftIsRejected();
  BinaryResultFastPathBuildsFrameAndPassesCleanPolicy();
  BinaryFastPathDisabledRecordsExactFallback();
  BinaryFastPathRequiresRuntimeConsumptionAndDiagnostics();
  return 0;
}
