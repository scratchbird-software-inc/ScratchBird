// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "nosql/columnar_api.hpp"
#include "nosql/spatial_api.hpp"

#if defined(SB_CES05_SPATIAL_COLUMNAR_PRODUCTION_QUERY_ROUTE)
#include "canonical_query_execute.hpp"
#include "crud_support/crud_store.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "memory.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"
#include "nosql/search_api.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace nosql = scratchbird::engine::internal_api::nosql;
namespace exec = scratchbird::engine::executor;
#if defined(SB_CES05_SPATIAL_COLUMNAR_PRODUCTION_QUERY_ROUTE)
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace memory = scratchbird::core::memory;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
#endif

namespace {

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "019f0000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-SPATIAL-COLUMNAR: " << detail << '\n';
  return condition;
}

template <typename Result>
bool HasEvidence(const Result& result, const std::string_view kind,
                 const std::string_view value) {
  return std::ranges::any_of(result.api_result.evidence,
                             [&](const auto& evidence) {
                               return evidence.evidence_kind == kind &&
                                      evidence.evidence_id == value;
                             });
}

exec::PhysicalMgaStatementContext Mga() {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(1);
  context.statement_timestamp = "2026-08-11T20:00:00Z";
  context.owning_transaction_uuid = Uuid(2);
  context.statement_snapshot_uuid = Uuid(3);
  context.statement_metadata_snapshot_uuid = Uuid(4);
  context.owning_local_transaction_id = 40;
  context.visible_committed_high_watermark = 39;
  context.oldest_active_transaction_id = 30;
  context.oldest_interesting_transaction_id = 29;
  context.oldest_snapshot_transaction_id = 29;
  context.retention_horizon_transaction_id = 29;
  context.active_excluded_local_transaction_ids = {40};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 41;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

nosql::SpatialExecutionRequestV1 SpatialRequest(
    const std::string& operation) {
  nosql::SpatialExecutionRequestV1 request;
  request.operation_id = operation;
  request.predicate_id = "INTERSECTS";
  request.object_uuid = Uuid(10);
  request.geometry_descriptor_uuid = Uuid(11);
  request.geometry_type_uuid = Uuid(12);
  request.crs_uuid = Uuid(13);
  request.query_crs_uuid = request.crs_uuid;
  request.crs_generation = 1;
  request.source_generation = 2;
  request.catalog_generation = 3;
  request.policy_generation = 4;
  request.security_generation = 5;
  request.resource_generation = 6;
  request.route_generation = 7;
  request.statement_context = Mga();
  request.current_statement_context = request.statement_context;
  request.source_rows = {
      {Uuid(101), nosql::EncodeSpatialPoint2dV1({0, 0}), request.crs_uuid},
      {Uuid(102), nosql::EncodeSpatialPoint2dV1({3, 4}), request.crs_uuid},
      {Uuid(103), nosql::EncodeSpatialPoint2dV1({0, 0}), request.crs_uuid},
  };
  request.encoded_query_point = nosql::EncodeSpatialPoint2dV1({0, 0});
  request.top_k = 3;
  request.maximum_rows = 16;
  request.security_admitted = true;
  request.exact_scan_fallback_available = true;
  return request;
}

bool SpatialVectors() {
  bool passed = true;
  std::set<std::string> completed;
  const auto credit = [&](const std::string_view case_id,
                          const bool condition,
                          const std::string_view detail) {
    passed &= Require(condition, detail);
    if (condition && completed.insert(std::string(case_id)).second) {
      std::cout << "RCP-079 literal catalog case=" << case_id
                << ";status=passed;skipped=0\n";
    }
  };
  const auto source = nosql::ExecuteSpatialNativeV1(
      SpatialRequest("SPATIAL_SOURCE"));
  credit("RCP079-SV-001",
         source.accepted && source.rows.size() == 3 &&
             source.rows[0].row_uuid == Uuid(101) &&
             source.rows[2].row_uuid == Uuid(103),
         "SV-001 source order or row shape drifted");

  auto match_request = SpatialRequest("SPATIAL_MATCH");
  const auto intersects = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-002",
         intersects.accepted && intersects.rows.size() == 2 &&
             intersects.rows[0].row_uuid == Uuid(101) &&
             intersects.rows[1].row_uuid == Uuid(103),
         "SV-002 INTERSECTS result drifted");
  match_request.predicate_id = "CONTAINS";
  const auto contains = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-003", contains.accepted && contains.rows.size() == 2,
         "SV-003 CONTAINS result drifted");

  auto nearest_request = SpatialRequest("SPATIAL_NEAREST");
  const auto nearest = nosql::ExecuteSpatialNativeV1(nearest_request);
  credit("RCP079-SV-004",
         nearest.accepted && nearest.rows.size() == 3 &&
             nearest.rows[0].row_uuid == Uuid(101) &&
             nearest.rows[1].row_uuid == Uuid(103) &&
             nearest.rows[2].row_uuid == Uuid(102) &&
             nearest.rows[2].distance == 5.0,
         "SV-004 nearest order/distance drifted");
  nearest_request.top_k = 2;
  const auto nearest_two = nosql::ExecuteSpatialNativeV1(nearest_request);
  credit("RCP079-SV-005",
         nearest_two.accepted && nearest_two.rows.size() == 2 &&
             nearest_two.rows.back().row_uuid == Uuid(103),
         "SV-005 exact top-k drifted");
  match_request.encoded_query_point =
      nosql::EncodeSpatialPoint2dV1({9, 9});
  const auto empty = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-006", empty.accepted && empty.rows.empty(),
         "SV-006 empty match was not an exact empty batch");

  nearest_request.query_crs_uuid = Uuid(14);
  const auto crs_mismatch = nosql::ExecuteSpatialNativeV1(nearest_request);
  credit("RCP079-SV-007",
         !crs_mismatch.accepted && crs_mismatch.rows.empty() &&
             !crs_mismatch.root_publishable &&
             crs_mismatch.diagnostic_id ==
                 "SB_MODEL_JOIN_SPATIAL_CRS_REFUSED_V1",
         "SV-007 CRS mismatch was not refused");
  match_request = SpatialRequest("SPATIAL_MATCH");
  match_request.query_crs_uuid.clear();
  const auto inferred = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-008",
         !inferred.accepted && inferred.rows.empty() &&
             inferred.diagnostic_id ==
                 "SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
         "SV-008 inferred CRS was admitted");
  match_request = SpatialRequest("SPATIAL_MATCH");
  match_request.predicate_id = "TOUCHES";
  const auto touches = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-009",
         !touches.accepted && touches.rows.empty() &&
             touches.diagnostic_id ==
                 "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
         "SV-009 unapproved topology predicate was admitted");
  auto line = SpatialRequest("SPATIAL_SOURCE");
  line.source_rows.front().value_profile_id = "LINESTRING_NATIVE_V1";
  const auto line_result = nosql::ExecuteSpatialNativeV1(line);
  credit("RCP079-SV-010",
         !line_result.accepted && line_result.rows.empty() &&
             line_result.diagnostic_id ==
                 "SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
         "SV-010 LINESTRING profile was admitted");
  auto geography = SpatialRequest("SPATIAL_SOURCE");
  geography.source_rows.front().value_profile_id = "GEOGRAPHY_NATIVE_V1";
  const auto geography_result = nosql::ExecuteSpatialNativeV1(geography);
  credit("RCP079-SV-011",
         !geography_result.accepted && geography_result.rows.empty() &&
             geography_result.diagnostic_id ==
                 "SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
         "SV-011 GEOGRAPHY profile was admitted");
  const auto nonfinite_refused = [](const double coordinate) {
    auto request = SpatialRequest("SPATIAL_SOURCE");
    request.source_rows.front().encoded_point =
        nosql::EncodeSpatialPoint2dV1({coordinate, 0});
    const auto result = nosql::ExecuteSpatialNativeV1(request);
    return !result.accepted && result.rows.empty() &&
           result.diagnostic_id ==
               "SB_MODEL_SPATIAL_COORDINATE_INVALID_V1";
  };
  credit("RCP079-SV-012",
         nonfinite_refused(std::numeric_limits<double>::quiet_NaN()) &&
             nonfinite_refused(std::numeric_limits<double>::infinity()) &&
             nonfinite_refused(-std::numeric_limits<double>::infinity()),
         "SV-012 NaN/+Inf/-Inf point was admitted");
  nearest_request = SpatialRequest("SPATIAL_NEAREST");
  nearest_request.top_k = 0;
  const auto zero_k = nosql::ExecuteSpatialNativeV1(nearest_request);
  nearest_request.top_k = 4097;
  const auto excess_k = nosql::ExecuteSpatialNativeV1(nearest_request);
  credit("RCP079-SV-013",
         !zero_k.accepted && !excess_k.accepted && zero_k.rows.empty() &&
             excess_k.rows.empty() &&
             zero_k.diagnostic_id == "SB_MODEL_SPATIAL_TOP_K_REFUSED_V1" &&
             excess_k.diagnostic_id ==
                 "SB_MODEL_SPATIAL_TOP_K_REFUSED_V1",
         "SV-013 top-k bounds were not exact");
  match_request = SpatialRequest("SPATIAL_MATCH");
  const auto fallback = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-014",
         fallback.accepted && fallback.exact_fallback_selected &&
             fallback.candidate_recheck_complete &&
             fallback.physical_operator_id ==
                 "SPATIAL_EXACT_GEOMETRY_SCAN_V1" &&
             fallback.rows.size() == 2,
         "SV-014 absent candidate did not select exact scan");
  match_request.candidate_proof =
      {true, true, true, true, true, true, {0, 1, 2}};
  const auto false_positive = nosql::ExecuteSpatialNativeV1(match_request);
  credit("RCP079-SV-015",
         false_positive.accepted &&
             !false_positive.exact_fallback_selected &&
             false_positive.candidate_recheck_complete &&
             false_positive.rows.size() == 2,
         "SV-015 candidate false positive escaped exact recheck");
  const std::set<std::string> expected{
      "RCP079-SV-001", "RCP079-SV-002", "RCP079-SV-003",
      "RCP079-SV-004", "RCP079-SV-005", "RCP079-SV-006",
      "RCP079-SV-007", "RCP079-SV-008", "RCP079-SV-009",
      "RCP079-SV-010", "RCP079-SV-011", "RCP079-SV-012",
      "RCP079-SV-013", "RCP079-SV-014", "RCP079-SV-015"};
  passed &= Require(completed == expected,
                    "RCP-079 spatial catalog is not the exact 15/15 set");
  if (completed == expected) {
    std::cout << "RCP-079 spatial catalog passed "
                 "(15/15;skipped=0)\n";
  }
  return passed;
}

api::EngineDescriptor Descriptor(const std::string& name,
                                 const std::string& type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = Uuid(200 + name.size() + type.size());
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor = "canonical=" + type;
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded, const bool nullable = false,
                            const bool missing = false) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.setState(missing ? api::EngineValueState::missing
                         : (nullable ? api::EngineValueState::sql_null
                                     : api::EngineValueState::value));
  return value;
}

nosql::ColumnarExecutionRequestV1 ColumnarRequest(
    const std::string& operation) {
  nosql::ColumnarExecutionRequestV1 request;
  request.operation_ids = {"COLUMNAR_SOURCE"};
  if (operation != "COLUMNAR_SOURCE") {
    request.operation_ids.push_back(operation);
  }
  request.operation_id = operation;
  request.relation_uuid = Uuid(210);
  request.row_uuids = {Uuid(211), Uuid(212), Uuid(213)};
  const auto uuid = Descriptor("row_uuid", "uuid");
  const auto integer = Descriptor("join_key", "int64");
  const auto text = Descriptor("payload", "text");
  request.logical_rows.columns = {
      {"row_uuid", uuid, false, 1},
      {"join_key", integer, true, 2},
      {"payload", text, true, 3},
  };
  request.logical_rows.rows = {
      {{Value(uuid, Uuid(211)), Value(integer, "1"), Value(text, "alpha")}},
      {{Value(uuid, Uuid(212)), Value(integer, "2"), Value(text, {}, true)}},
      {{Value(uuid, Uuid(213)), Value(integer, {}, true), Value(text, "beta")}},
  };
  request.statement_context = Mga();
  request.current_statement_context = request.statement_context;
  request.source_generation = 1;
  request.catalog_generation = 2;
  request.summary_generation = 3;
  request.maximum_rows = 16;
  request.maximum_cells = 64;
  request.security_admitted = true;
  request.exact_reconstruction_fallback_available = true;
  return request;
}

bool ColumnarVectors() {
  bool passed = true;
  std::set<std::string> completed;
  const auto credit = [&](const std::string_view case_id,
                          const bool condition,
                          const std::string_view detail) {
    passed &= Require(condition, detail);
    if (condition && completed.insert(std::string(case_id)).second) {
      std::cout << "RCP-079 literal catalog case=" << case_id
                << ";status=passed;skipped=0\n";
    }
  };
  const auto source = nosql::ExecuteColumnarLogicalV1(
      ColumnarRequest("COLUMNAR_SOURCE"));
  credit("RCP079-CV-001",
         source.accepted && source.batch.rows.size() == 3 &&
             source.batch.columns.size() == 3 &&
             source.row_uuids ==
                 std::vector<std::string>{Uuid(211), Uuid(212), Uuid(213)},
         "CV-001 source reconstruction drifted");
  auto project = ColumnarRequest("COLUMNAR_PROJECT");
  project.projected_columns = {2, 0};
  const auto projected = nosql::ExecuteColumnarLogicalV1(project);
  credit("RCP079-CV-002",
         projected.accepted && projected.batch.columns.size() == 2 &&
             projected.batch.columns[0].stable_name == "payload" &&
             projected.batch.columns[1].stable_name == "row_uuid",
         "CV-002 projection order/descriptor drifted");
  auto filter = ColumnarRequest("COLUMNAR_FILTER");
  filter.filter_truth_values = {api::EngineSqlTruthValue::false_value,
                                api::EngineSqlTruthValue::true_value,
                                api::EngineSqlTruthValue::unknown};
  const auto filtered = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-003",
         filtered.accepted && filtered.batch.rows.size() == 1 &&
             filtered.row_uuids.front() == Uuid(212),
         "CV-003 join_key > 1 truth/result drifted");
  auto null_filter = ColumnarRequest("COLUMNAR_FILTER");
  null_filter.filter_truth_values = {api::EngineSqlTruthValue::false_value,
                                     api::EngineSqlTruthValue::true_value,
                                     api::EngineSqlTruthValue::false_value};
  const auto null_filtered = nosql::ExecuteColumnarLogicalV1(null_filter);
  credit("RCP079-CV-004",
         null_filtered.accepted && null_filtered.batch.rows.size() == 1 &&
             null_filtered.row_uuids.front() == Uuid(212),
         "CV-004 IS NULL truth carriage drifted");
  auto alpha_filter = ColumnarRequest("COLUMNAR_FILTER");
  alpha_filter.filter_truth_values = {api::EngineSqlTruthValue::true_value,
                                      api::EngineSqlTruthValue::unknown,
                                      api::EngineSqlTruthValue::false_value};
  const auto alpha_filtered =
      nosql::ExecuteColumnarLogicalV1(alpha_filter);
  credit("RCP079-CV-005",
         alpha_filtered.accepted && alpha_filtered.batch.rows.size() == 1 &&
             alpha_filtered.row_uuids.front() == Uuid(211),
         "CV-005 payload = alpha truth/result drifted");
  auto filter_project = ColumnarRequest("COLUMNAR_FILTER");
  filter_project.operation_ids.push_back("COLUMNAR_PROJECT");
  filter_project.operation_id.clear();
  filter_project.filter_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown};
  filter_project.projected_columns = {2, 0};
  const auto filtered_projected =
      nosql::ExecuteColumnarLogicalV1(filter_project);
  passed &= Require(
      filtered_projected.accepted &&
          filtered_projected.batch.columns.size() == 2 &&
          filtered_projected.batch.columns[0].stable_name == "payload" &&
          filtered_projected.batch.columns[1].stable_name == "row_uuid" &&
          filtered_projected.batch.rows.size() == 1 &&
          filtered_projected.row_uuids ==
              std::vector<std::string>{Uuid(211)},
      "CV-004A FILTER then PROJECT composition drifted");
  auto composite_operation = filter_project;
  composite_operation.operation_id = "COLUMNAR_FILTER_PROJECT";
  const auto composite_refused =
      nosql::ExecuteColumnarLogicalV1(composite_operation);
  passed &= Require(
      !composite_refused.accepted && !composite_refused.root_publishable &&
          composite_refused.diagnostic_id ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
      "invented composite singular operation ID was admitted");
  auto reordered_operations = filter_project;
  reordered_operations.operation_ids = {
      "COLUMNAR_SOURCE", "COLUMNAR_PROJECT", "COLUMNAR_FILTER"};
  const auto reordered_refused =
      nosql::ExecuteColumnarLogicalV1(reordered_operations);
  passed &= Require(
      !reordered_refused.accepted && !reordered_refused.root_publishable &&
          reordered_refused.diagnostic_id ==
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
      "out-of-order columnar operation roots were admitted");
  project.projected_columns = {2, 2};
  const auto duplicate = nosql::ExecuteColumnarLogicalV1(project);
  credit("RCP079-CV-006",
         !duplicate.accepted && duplicate.diagnostic_id ==
                                    "SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
         "CV-006 duplicate projection was admitted");
  filter = ColumnarRequest("COLUMNAR_FILTER");
  filter.filter_truth_values = {api::EngineSqlTruthValue::false_value,
                                api::EngineSqlTruthValue::true_value,
                                api::EngineSqlTruthValue::unknown};
  filter.lossy_coercion_requested = true;
  const auto lossy = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-007",
         !lossy.accepted && lossy.diagnostic_id ==
                                "SB_MODEL_JOIN_LOSSY_COERCION_REFUSED_V1",
         "CV-007 lossy coercion was admitted");
  auto dictionary = ColumnarRequest("COLUMNAR_SOURCE");
  dictionary.representation = nosql::ColumnarTestRepresentationV1::kDictionary;
  auto rle = dictionary;
  rle.representation = nosql::ColumnarTestRepresentationV1::kRunLength;
  const auto dictionary_result = nosql::ExecuteColumnarLogicalV1(dictionary);
  const auto rle_result = nosql::ExecuteColumnarLogicalV1(rle);
  credit("RCP079-CV-008",
         dictionary_result.accepted && rle_result.accepted &&
             dictionary_result.row_uuids == rle_result.row_uuids &&
             dictionary_result.batch.rows.size() ==
                 rle_result.batch.rows.size(),
         "CV-008 encoding-independent reconstruction drifted");
  filter = ColumnarRequest("COLUMNAR_FILTER");
  filter.filter_truth_values = {api::EngineSqlTruthValue::false_value,
                                api::EngineSqlTruthValue::true_value,
                                api::EngineSqlTruthValue::unknown};
  filter.zone_proof = {true, true, true, true, true, true, true, true, true,
                       false, {1}};
  const auto pruned = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-009",
         pruned.accepted && !pruned.exact_fallback_selected &&
             pruned.batch.rows.size() == 1,
         "CV-009 fresh zone candidate was not exactly rechecked");
  filter.zone_proof.fresh = false;
  const auto stale = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-010",
         stale.accepted && stale.exact_fallback_selected &&
             stale.batch.rows.size() == 1,
         "CV-010 stale zone did not fall back");
  filter = ColumnarRequest("COLUMNAR_FILTER");
  filter.filter_truth_values = {api::EngineSqlTruthValue::false_value,
                                api::EngineSqlTruthValue::true_value,
                                api::EngineSqlTruthValue::unknown};
  filter.zone_proof = {true, true, true, true, true, true, true, true, true,
                       false, {0, 1, 2}};
  const auto lossy_zone = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-011",
         lossy_zone.accepted && lossy_zone.batch.rows.size() == 1,
         "CV-011 zone false positive escaped predicate recheck");
  auto encoding_leak = ColumnarRequest("COLUMNAR_SOURCE");
  encoding_leak.logical_rows.columns[2].stable_name = "dictionary_id";
  const auto leak = nosql::ExecuteColumnarLogicalV1(encoding_leak);
  credit("RCP079-CV-012",
         !leak.accepted && leak.diagnostic_id ==
                               "SB_MODEL_COLUMNAR_ENCODING_LEAK_REFUSED_V1",
         "CV-012 physical encoding leaked to public columns");
  filter.zone_proof.comparison_exact = false;
  filter.zone_proof.snapshot_safe = true;
  const auto profile_mismatch = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-013",
         profile_mismatch.accepted &&
             profile_mismatch.exact_fallback_selected &&
             profile_mismatch.fallback_reason_id ==
                 "ZONE.COMPARISON_PROFILE_MISMATCH",
         "CV-013 comparison-profile mismatch did not fall back");
  filter.zone_proof.comparison_exact = true;
  filter.zone_proof.snapshot_safe = false;
  const auto unsafe = nosql::ExecuteColumnarLogicalV1(filter);
  credit("RCP079-CV-014",
         unsafe.accepted && unsafe.exact_fallback_selected &&
             unsafe.fallback_reason_id == "ZONE.MGA_SNAPSHOT_UNSAFE",
         "CV-014 MGA-unsafe zone did not fall back");
  const std::set<std::string> expected{
      "RCP079-CV-001", "RCP079-CV-002", "RCP079-CV-003",
      "RCP079-CV-004", "RCP079-CV-005", "RCP079-CV-006",
      "RCP079-CV-007", "RCP079-CV-008", "RCP079-CV-009",
      "RCP079-CV-010", "RCP079-CV-011", "RCP079-CV-012",
      "RCP079-CV-013", "RCP079-CV-014"};
  passed &= Require(completed == expected,
                    "RCP-079 columnar catalog is not the exact 14/14 set");
  if (completed == expected) {
    std::cout << "RCP-079 columnar catalog passed "
                 "(14/14;skipped=0)\n";
  }
  return passed;
}

#if defined(SB_CES05_SPATIAL_COLUMNAR_PRODUCTION_QUERY_ROUTE)
std::uint64_t ProductionNowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string ProductionUuid(const platform::UuidKind kind,
                           const std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, ProductionNowMillis() + salt);
  return generated.ok() ? uuid::UuidToString(generated.value.value)
                        : std::string{};
}

std::string ProductionCoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(found->descriptor_uuid.value);
}

void AppendProductionLittleEndianU64(std::vector<std::uint8_t>* output,
                                     const std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void AddProductionDecoderOperand(sblr::SblrOperationEnvelope* envelope,
                                 std::string type, std::string name,
                                 const std::string_view value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.ordinal =
      static_cast<std::uint32_t>(envelope->operands.size() + 1);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(16, 0);
  operand.value_body.front() = 0x73;
  AppendProductionLittleEndianU64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(), value.end());
  envelope->operands.push_back(std::move(operand));
}

void SetProductionDecoderOperandValue(sblr::SblrOperand* operand,
                                      const std::string_view value) {
  if (operand == nullptr) return;
  operand->value.clear();
  operand->value_kind = sblr::SblrValueKind::literal_typed;
  operand->value_body.assign(16, 0);
  operand->value_body.front() = 0x73;
  AppendProductionLittleEndianU64(&operand->value_body, value.size());
  operand->value_body.insert(operand->value_body.end(), value.begin(), value.end());
}

std::string ProductionDecoderHex(const std::string_view value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    encoded.push_back(kDigits[byte >> 4]);
    encoded.push_back(kDigits[byte & 0x0f]);
  }
  return encoded;
}

enum class ProductionTimestampDecoderFamily {
  key_value,
  time_series,
  vector,
  search,
  spatial,
  columnar,
  mixed_spatial_columnar,
  document,
  graph,
  ordinary_relational,
};

sblr::SblrOperationEnvelope ProductionTimestampDecoderEnvelope(
    const api::EngineRequestContext& context,
    const ProductionTimestampDecoderFamily family) {
  const auto* operation = sblr::LookupSblrOperation("query.execute");
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "rcp079.timestamp-model.decoder-reconciliation.v1");
  envelope.opcode_code = operation == nullptr ? 0 : operation->code;
  envelope.parser_package_uuid =
      "019f0790-0000-7000-8000-000000000001";
  envelope.registry_snapshot_uuid =
      "019f0790-0000-7000-8000-000000000002";
  envelope.result_shape = "query_execute_result";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;

  AddProductionDecoderOperand(&envelope, "uint16", "relational_wire_version",
                              "2");
  AddProductionDecoderOperand(&envelope, "uuid",
                              "relational_bound_sblr_tree_uuid",
                              ProductionUuid(platform::UuidKind::object, 79001));
  AddProductionDecoderOperand(&envelope, "uuid",
                              "relational_catalog_epoch_uuid",
                              context.catalog_epoch_uuid.canonical);
  AddProductionDecoderOperand(
      &envelope, "uuid", "relational_security_context_uuid",
      context.authorization_context.authority_uuid.canonical);
  AddProductionDecoderOperand(&envelope, "uuid", "relational_statement_uuid",
                              context.statement_uuid.canonical);
  AddProductionDecoderOperand(&envelope, "uuid",
                              "relational_owning_transaction_uuid",
                              context.transaction_uuid.canonical);
  AddProductionDecoderOperand(&envelope, "uuid",
                              "relational_statement_snapshot_uuid",
                              context.statement_snapshot_uuid.canonical);
  AddProductionDecoderOperand(
      &envelope, "uuid", "relational_statement_metadata_snapshot_uuid",
      context.statement_metadata_snapshot_uuid.canonical);
  AddProductionDecoderOperand(
      &envelope, "uint64", "relational_local_transaction_id",
      std::to_string(context.local_transaction_id));
  AddProductionDecoderOperand(
      &envelope, "uint64",
      "relational_snapshot_visible_through_local_transaction_id",
      std::to_string(context.snapshot_visible_through_local_transaction_id));
  AddProductionDecoderOperand(&envelope, "text",
                              "relational_statement_timestamp",
                              context.statement_timestamp);

  const bool mixed =
      family == ProductionTimestampDecoderFamily::mixed_spatial_columnar;
  AddProductionDecoderOperand(&envelope, "uint32", "relational_root_node_id",
                              mixed ? "3" : "1");
  const auto add_expression = [&](const std::uint32_t id,
                                  const std::string_view name,
                                  const std::string_view bound_object = {}) {
    AddProductionDecoderOperand(
        &envelope, "relational_expression_v1", "slot_" + std::to_string(id),
        "4|-|1|-|" +
            (bound_object.empty() ? std::string("-")
                                  : std::string(bound_object)) +
            "|-|" + ProductionDecoderHex(name) + "|-");
  };
  const auto add_node = [&](const std::uint32_t id, const std::uint8_t kind,
                            const std::string_view inputs,
                            const std::string_view descriptors,
                            const std::string_view semantic,
                            const std::string_view expressions,
                            const std::string_view object) {
    AddProductionDecoderOperand(
        &envelope, "relational_node_v1", "slot_" + std::to_string(id),
        std::to_string(kind) + "|0|" + std::string(inputs) + "|" +
            std::string(descriptors) + "|-");
    AddProductionDecoderOperand(
        &envelope, "relational_node_binding_v1",
        "slot_" + std::to_string(id),
        ProductionDecoderHex(semantic) + "|" + std::string(expressions) +
            "|" + std::string(object) + "|-|-");
  };
  const auto object_a = ProductionUuid(platform::UuidKind::object, 79011);
  const auto object_b = ProductionUuid(platform::UuidKind::object, 79012);
  switch (family) {
    case ProductionTimestampDecoderFamily::key_value:
      add_expression(100, "KV_KEY");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::time_series:
      add_expression(100, "TIME_RANGE");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::vector:
      add_expression(100, "VECTOR_NEAREST");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::search:
      add_expression(100, "SEARCH_MATCH");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::spatial:
      add_expression(100, "SPATIAL_SOURCE", object_a);
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::columnar:
      add_expression(100, "COLUMNAR_SOURCE", object_a);
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::mixed_spatial_columnar:
      add_expression(100, "SPATIAL_SOURCE", object_a);
      add_expression(200, "COLUMNAR_SOURCE", object_b);
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      add_node(2, 1, "-", "2", "SBLR_MODEL_SOURCE_V1", "200", object_b);
      add_node(3, 4, "1,2", "1,2", "join.inner.on.v1", "-", "-");
      break;
    case ProductionTimestampDecoderFamily::document:
      add_expression(100, "DOCUMENT_SOURCE");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::graph:
      add_expression(100, "GRAPH_MATCH");
      add_node(1, 1, "-", "1", "SBLR_MODEL_SOURCE_V1", "100", object_a);
      break;
    case ProductionTimestampDecoderFamily::ordinary_relational:
      add_node(1, 1, "-", "1", "relation.source.v1", "-", object_a);
      break;
  }
  return envelope;
}

const api::EngineApiDiagnostic* ProductionDecoderDiagnostic(
    const sblr::SblrDispatchResult& result) {
  return result.api_result.diagnostics.empty()
             ? nullptr
             : &result.api_result.diagnostics.front();
}

bool ProductionTimestampDecoderReconciliation(
    const api::EngineRequestContext& context) {
  constexpr std::array<ProductionTimestampDecoderFamily, 7> kAcceptedFamilies{
      ProductionTimestampDecoderFamily::key_value,
      ProductionTimestampDecoderFamily::time_series,
      ProductionTimestampDecoderFamily::vector,
      ProductionTimestampDecoderFamily::search,
      ProductionTimestampDecoderFamily::spatial,
      ProductionTimestampDecoderFamily::columnar,
      ProductionTimestampDecoderFamily::mixed_spatial_columnar};
  bool passed = true;
  const auto pre_optimizer_refusal = [&](const sblr::SblrDispatchResult& result,
                                         const std::string_view code) {
    const auto* diagnostic = ProductionDecoderDiagnostic(result);
    return result.envelope_validated && diagnostic != nullptr &&
           diagnostic->code == code && !result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed &&
           !result.runtime_actuals_attached &&
           !result.canonical_result_published;
  };
  constexpr std::array<std::string_view, 7> kLaterDiagnosticCodes{
      "SBLR.PLAN_TREE.INVALID_HANDLE",
      "SBLR.PLAN_TREE.INVALID_HANDLE",
      "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
      "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
      "SBLR.PLAN_TREE.INVALID_HANDLE",
      "SBLR.PLAN_TREE.INVALID_HANDLE",
      "SBLR.PLAN_TREE.INVALID_HANDLE"};
  constexpr std::array<std::string_view, 7> kLaterDiagnosticFields{
      "expression_record", "expression_record",
      "vector_model_operation_identity", "search_model_operation_identity",
      "expression_record", "expression_record", "expression_record"};
  for (std::size_t ordinal = 0; ordinal < kAcceptedFamilies.size(); ++ordinal) {
    const auto family = kAcceptedFamilies[ordinal];
    const auto admitted = sblr::DispatchSblrOperation(
        {context, ProductionTimestampDecoderEnvelope(context, family), {}});
    const auto* diagnostic = ProductionDecoderDiagnostic(admitted);
    std::cerr << "QOW-CES05-SPATIAL-COLUMNAR decoder positive family="
              << static_cast<unsigned>(family) << ":"
              << (diagnostic == nullptr ? std::string("no-diagnostic")
                                        : diagnostic->code + ":" +
                                              diagnostic->detail)
              << ":flags=" << admitted.envelope_validated << ','
              << admitted.optimizer_admitted << ','
              << admitted.optimizer_selected << ','
              << admitted.physical_dag_published << ','
              << admitted.physical_dag_executed << ','
              << admitted.canonical_result_published << '\n';
    passed &= Require(
        pre_optimizer_refusal(admitted, kLaterDiagnosticCodes[ordinal]) &&
            diagnostic != nullptr &&
            diagnostic->detail.starts_with(kLaterDiagnosticFields[ordinal]),
        "exact timestamp family did not traverse decoder into typed-DAG "
        "validation");
  }

  const auto dispatch = [&](sblr::SblrOperationEnvelope envelope) {
    return sblr::DispatchSblrOperation({context, std::move(envelope), {}});
  };
  const auto timestamp_operand = [](sblr::SblrOperationEnvelope* envelope) {
    return std::ranges::find_if(envelope->operands, [](const auto& operand) {
      return operand.name == "relational_statement_timestamp";
    });
  };
  const auto binding_operand = [](sblr::SblrOperationEnvelope* envelope,
                                  const std::string_view slot) {
    return std::ranges::find_if(envelope->operands, [&](const auto& operand) {
      return operand.type == "relational_node_binding_v1" &&
             operand.name == slot;
    });
  };
  const auto node_operand = [](sblr::SblrOperationEnvelope* envelope,
                               const std::string_view slot) {
    return std::ranges::find_if(envelope->operands, [&](const auto& operand) {
      return operand.type == "relational_node_v1" && operand.name == slot;
    });
  };
  const auto expect = [&](const std::string_view id,
                          sblr::SblrOperationEnvelope envelope,
                          const std::string_view diagnostic) {
    const auto result = dispatch(std::move(envelope));
    passed &= Require(pre_optimizer_refusal(result, diagnostic),
                      "timestamp decoder mutation crossed pre-optimizer: " +
                          std::string(id));
  };

  auto missing = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  missing.operands.erase(timestamp_operand(&missing));
  for (std::size_t ordinal = 0; ordinal < missing.operands.size(); ++ordinal) {
    missing.operands[ordinal].ordinal = static_cast<std::uint32_t>(ordinal + 1);
  }
  expect("missing", std::move(missing), "SB_MODEL_MGA_CONTEXT_MISMATCH_V1");

  auto duplicate = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  AddProductionDecoderOperand(&duplicate, "text",
                              "relational_statement_timestamp",
                              context.statement_timestamp);
  expect("duplicate", std::move(duplicate),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto reordered = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  std::swap(reordered.operands[10], reordered.operands[11]);
  reordered.operands[10].ordinal = 11;
  reordered.operands[11].ordinal = 12;
  expect("reordered", std::move(reordered),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto mistyped = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  timestamp_operand(&mistyped)->type = "uint64";
  expect("mistyped", std::move(mistyped),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto malformed = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  SetProductionDecoderOperandValue(&*timestamp_operand(&malformed),
                                   "2026-02-30T25:61:61Z");
  expect("malformed", std::move(malformed),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  for (const auto [id, family] :
       std::array<std::pair<std::string_view,
                            ProductionTimestampDecoderFamily>,
                  3>{{{"extra-document",
                       ProductionTimestampDecoderFamily::document},
                      {"extra-graph", ProductionTimestampDecoderFamily::graph},
                      {"extra-relational",
                       ProductionTimestampDecoderFamily::ordinary_relational}}}) {
    expect(id, ProductionTimestampDecoderEnvelope(context, family),
           "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
  }

  auto mismatch = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  SetProductionDecoderOperandValue(&*timestamp_operand(&mismatch),
                                   "2026-08-11T20:00:01Z");
  expect("context-mismatch", std::move(mismatch),
         "SB_MODEL_MGA_CONTEXT_MISMATCH_V1");

  auto unattached = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::spatial);
  SetProductionDecoderOperandValue(&*binding_operand(&unattached, "slot_1"),
                                   ProductionDecoderHex("SBLR_MODEL_SOURCE_V1") +
                                       "|-|" +
                                       ProductionUuid(platform::UuidKind::object,
                                                      79011) +
                                       "|-|-");
  expect("unattached-root", std::move(unattached),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto cross_family = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::spatial);
  AddProductionDecoderOperand(&cross_family, "relational_expression_v1",
                              "slot_101", "4|-|1|-|-|-|" +
                                              ProductionDecoderHex("KV_KEY") +
                                              "|-");
  SetProductionDecoderOperandValue(
      &*binding_operand(&cross_family, "slot_1"),
      ProductionDecoderHex("SBLR_MODEL_SOURCE_V1") + "|100,101|" +
          ProductionUuid(platform::UuidKind::object, 79011) + "|-|-");
  expect("cross-family", std::move(cross_family),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto root_order = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  SetProductionDecoderOperandValue(&*node_operand(&root_order, "slot_3"),
                                   "4|0|2,1|1,2|-");
  expect("root-order", std::move(root_order),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto empty_descriptors = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  SetProductionDecoderOperandValue(
      &*node_operand(&empty_descriptors, "slot_1"), "1|0|-|-|-");
  expect("empty-leg-descriptors", std::move(empty_descriptors),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  auto overlapping_descriptors = ProductionTimestampDecoderEnvelope(
      context, ProductionTimestampDecoderFamily::mixed_spatial_columnar);
  SetProductionDecoderOperandValue(
      &*node_operand(&overlapping_descriptors, "slot_2"), "1|0|-|1|-");
  expect("overlapping-leg-descriptors", std::move(overlapping_descriptors),
         "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
  return passed;
}

std::vector<opt::MultilegDescriptorProfileV1>
ProductionMultilegProfiles(const std::uint64_t salt) {
  const std::array<std::string, 5> types = {
      ProductionCoreTypeUuid("uuid"), ProductionCoreTypeUuid("uint64"),
      ProductionCoreTypeUuid("real64"), ProductionCoreTypeUuid("boolean"),
      ProductionCoreTypeUuid("geometry")};
  std::vector<opt::MultilegDescriptorProfileV1> profiles;
  profiles.reserve(320);
  for (std::uint16_t pair = 0; pair < types.size(); ++pair) {
    for (std::uint16_t nullable = 0; nullable < 2; ++nullable) {
      const auto kind = static_cast<std::uint8_t>(14 + pair * 2 + nullable);
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        profiles.push_back(
            {kind, slot,
             ProductionUuid(platform::UuidKind::object,
                            salt + 1000 + profiles.size()),
             types[pair], nullable != 0});
      }
    }
  }
  return profiles;
}

bool ProductionDescriptorScopeLifecycle() {
  const auto profiles =
      ProductionMultilegProfiles(ProductionNowMillis() % 1'000'000);
  const auto statement_uuid =
      ProductionUuid(platform::UuidKind::object, 700'001);
  const auto other_statement_uuid =
      ProductionUuid(platform::UuidKind::object, 700'002);
  const auto absent =
      opt::LookupMultilegDescriptorDispatchScopeV1(statement_uuid);
  bool exact = !absent.accepted && absent.profiles.empty() &&
               absent.diagnostic_id ==
                   "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1";
  {
    opt::MultilegDescriptorDispatchScopeV1 scope(statement_uuid, profiles);
    const auto active =
        opt::LookupMultilegDescriptorDispatchScopeV1(statement_uuid);
    const auto mismatch =
        opt::LookupMultilegDescriptorDispatchScopeV1(other_statement_uuid);
    opt::MultilegDescriptorDispatchScopeV1 nested(statement_uuid, profiles);
    opt::MultilegDescriptorDispatchScopeV1 conflicting(
        other_statement_uuid, profiles);
    exact = exact && scope.installed() && active.accepted &&
            active.profiles.size() == 320 && !mismatch.accepted &&
            mismatch.diagnostic_id ==
                "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_MISMATCH_V1" &&
            !nested.installed() && !conflicting.installed() &&
            nested.diagnostic_id() ==
                "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_NESTED_V1" &&
            conflicting.diagnostic_id() ==
                "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_NESTED_V1";
  }
  const auto cleared =
      opt::LookupMultilegDescriptorDispatchScopeV1(statement_uuid);
  return Require(exact && !cleared.accepted && cleared.profiles.empty() &&
                     cleared.diagnostic_id ==
                         "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1",
                 "production V10 descriptor scope lifecycle drifted");
}

struct ProductionFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string relation_uuid;
  std::uint64_t salt{0};

  ~ProductionFixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

api::EngineRequestContext ProductionBaseContext(
    const ProductionFixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

bool ProductionBegin(const ProductionFixture& fixture, std::string request_id,
                     api::EngineRequestContext* context) {
  if (context == nullptr) return false;
  api::EngineBeginTransactionRequest request;
  request.context = ProductionBaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) return false;
  *context = request.context;
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool ProductionCommit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return api::EngineCommitTransaction(request).ok;
}

bool ProductionRollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request).ok;
}

bool ProductionPublishSnapshot(api::EngineRequestContext* context,
                               const std::uint64_t salt) {
  if (context == nullptr) return false;
  context->statement_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, salt);
  api::EnginePublishStatementSnapshotRequest request;
  request.context = *context;
  const auto published = api::EnginePublishStatementSnapshot(request);
  if (!published.ok) return false;
  context->statement_snapshot_uuid = published.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      published.snapshot_vector.visible_committed_high_watermark;
  return true;
}

void AddProductionAuthorization(api::EngineRequestContext* context,
                                const std::string& object_uuid) {
  auto& authorization = context->authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, 9001);
  authorization.principal_uuid = context->principal_uuid;
  authorization.security_epoch = context->security_epoch;
  authorization.policy_epoch = 1;
  authorization.catalog_generation_id = context->catalog_generation_id;
  authorization.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      ProductionUuid(platform::UuidKind::object,
                     9002 + authorization.grants.size());
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = object_uuid;
  grant.right = "SELECT";
  grant.security_epoch = context->security_epoch;
  authorization.grants.push_back(std::move(grant));
}

std::string DescriptorTypeUuid(const api::EngineDescriptor& descriptor) {
  constexpr std::string_view prefix = "type_uuid=";
  const auto begin = descriptor.encoded_descriptor.find(prefix);
  if (begin == std::string::npos) return {};
  const auto value_begin = begin + prefix.size();
  const auto end = descriptor.encoded_descriptor.find(';', value_begin);
  return descriptor.encoded_descriptor.substr(
      value_begin, end == std::string::npos ? std::string::npos
                                            : end - value_begin);
}

api::RelationalTypeDescriptor ProductionDescriptor(
    const std::uint32_t descriptor_id,
    const api::MgaRelationColumnStorageDescriptor& column) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid =
      column.value_descriptor.descriptor_uuid.canonical;
  descriptor.type_uuid = DescriptorTypeUuid(column.value_descriptor);
  descriptor.nullability = column.nullable
                               ? api::RelationalNullability::kNullable
                               : api::RelationalNullability::kNonNull;
  if (!column.collation_uuid.empty()) {
    descriptor.collation_uuid = column.collation_uuid;
  }
  if (column.character_length != 0) {
    descriptor.width = column.character_length;
  }
  return descriptor;
}

api::TypedRelationalDag ProductionColumnarDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, 9100);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  for (std::size_t ordinal = 0; ordinal < storage.columns.size(); ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    const auto expression_id = static_cast<std::uint32_t>(1 + ordinal);
    dag.descriptors.push_back(
        ProductionDescriptor(descriptor_id, storage.columns[ordinal]));
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = descriptor_id;
    expression.bound_name_uuid =
        storage.columns[ordinal].column_uuid.canonical;
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1, expression_id,
         storage.columns[ordinal].canonical_name_key, descriptor_id, true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord source_expression;
  source_expression.expression_id = 100;
  source_expression.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  source_expression.result_descriptor_id = 101;
  source_expression.operator_name = "COLUMNAR_SOURCE";
  source_expression.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(source_expression));
  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  for (std::size_t ordinal = 0; ordinal < storage.columns.size(); ++ordinal) {
    source.output_descriptor_ids.push_back(
        static_cast<std::uint32_t>(101 + ordinal));
    source.bound_expression_ids.push_back(
        static_cast<std::uint32_t>(1 + ordinal));
  }
  source.bound_expression_ids.push_back(100);
  source.required_object_uuids = {storage.relation_uuid.canonical};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag ProductionColumnarJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& left,
    const api::MgaRelationStorageDescriptor& right, const std::uint64_t salt,
    const bool right_is_model = true,
    const std::string_view join_semantic = "join.inner.on.v1") {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, salt);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  const auto append_source = [&](const api::MgaRelationStorageDescriptor& storage,
                                 const std::uint32_t node_id,
                                 const std::uint32_t descriptor_base,
                                 const std::uint32_t expression_base,
                                 const std::uint32_t source_expression_id,
                                 const bool model_source) {
    api::RelationalDagNode source;
    source.node_id = node_id;
    source.node_kind = api::RelationalDagNodeKind::kScan;
    source.required_object_uuids = {storage.relation_uuid.canonical};
    source.semantic_variant_id =
        model_source ? "SBLR_MODEL_SOURCE_V1" : "relation.source.v1";
    for (std::size_t ordinal = 0; ordinal < storage.columns.size(); ++ordinal) {
      const auto descriptor_id =
          descriptor_base + static_cast<std::uint32_t>(ordinal);
      const auto expression_id =
          expression_base + static_cast<std::uint32_t>(ordinal);
      dag.descriptors.push_back(
          ProductionDescriptor(descriptor_id, storage.columns[ordinal]));
      api::RelationalExpressionRecord expression;
      expression.expression_id = expression_id;
      expression.expression_kind =
          api::RelationalExpressionKind::kIdentifier;
      expression.result_descriptor_id = descriptor_id;
      expression.bound_name_uuid =
          storage.columns[ordinal].column_uuid.canonical;
      dag.expressions.push_back(std::move(expression));
      source.output_descriptor_ids.push_back(descriptor_id);
      source.bound_expression_ids.push_back(expression_id);
      dag.outputs.push_back(
          {static_cast<std::uint32_t>(dag.outputs.size() + 1), node_id,
           expression_id, storage.columns[ordinal].canonical_name_key,
           descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
    }
    if (model_source) {
      api::RelationalExpressionRecord source_expression;
      source_expression.expression_id = source_expression_id;
      source_expression.expression_kind =
          api::RelationalExpressionKind::kFunctionCall;
      source_expression.result_descriptor_id = descriptor_base;
      source_expression.operator_name = "COLUMNAR_SOURCE";
      source_expression.bound_name_uuid = storage.relation_uuid.canonical;
      dag.expressions.push_back(std::move(source_expression));
      source.bound_expression_ids.push_back(source_expression_id);
    }
    dag.nodes.push_back(std::move(source));
  };
  append_source(left, 1, 101, 1, 100, true);
  append_source(right, 2, 201, 11, 200, right_is_model);

  const auto boolean_type = ProductionCoreTypeUuid("boolean");
  api::RelationalTypeDescriptor boolean_descriptor;
  boolean_descriptor.descriptor_id = 301;
  boolean_descriptor.descriptor_uuid =
      ProductionUuid(platform::UuidKind::object, salt + 1);
  boolean_descriptor.type_uuid = boolean_type;
  boolean_descriptor.nullability =
      api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(boolean_descriptor));
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 300;
  predicate.expression_kind = api::RelationalExpressionKind::kBinary;
  predicate.child_expression_ids = {2, 12};
  predicate.result_descriptor_id = 301;
  predicate.operator_name = "=";
  dag.expressions.push_back(std::move(predicate));

  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  const bool left_only = join_semantic.starts_with("join.left-semi") ||
                         join_semantic.starts_with("join.left-anti");
  join.output_descriptor_ids = left_only
                                   ? std::vector<std::uint32_t>{101, 102, 103}
                                   : std::vector<std::uint32_t>{
                                         101, 102, 103, 201, 202, 203};
  if (!join_semantic.starts_with("join.lateral-") &&
      join_semantic != "join.cross.v1") {
    join.bound_expression_ids = {300};
  }
  join.semantic_variant_id = join_semantic;
  dag.nodes.push_back(std::move(join));
  static constexpr std::array<std::string_view, 6> kNames{
      "row_uuid", "join_key", "payload", "row_uuid", "join_key",
      "payload"};
  constexpr std::array<std::uint32_t, 6> kExpressions{1, 2, 3, 11, 12, 13};
  constexpr std::array<std::uint32_t, 6> kDescriptors{101, 102, 103,
                                                     201, 202, 203};
  for (std::size_t ordinal = 0;
       ordinal < (left_only ? 3U : kNames.size()); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(dag.outputs.size() + 1), 3,
         kExpressions[ordinal], std::string(kNames[ordinal]),
         kDescriptors[ordinal], true, static_cast<std::uint32_t>(ordinal)});
  }
  return dag;
}

api::TypedRelationalDag ProductionTimezoneEquivalentColumnarJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& left,
    const api::MgaRelationStorageDescriptor& right,
    const std::uint64_t salt) {
  auto dag = ProductionColumnarJoinDag(context, left, right, salt);
  const auto timestamp_type = ProductionCoreTypeUuid("timestamp");
  for (const auto descriptor_id : {302U, 303U}) {
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = descriptor_id;
    descriptor.descriptor_uuid = ProductionUuid(
        platform::UuidKind::object, salt + descriptor_id);
    descriptor.type_uuid = timestamp_type;
    descriptor.nullability = api::RelationalNullability::kNonNull;
    descriptor.timezone_profile_id = "timestamp_timezone_profile";
    dag.descriptors.push_back(std::move(descriptor));
  }
  api::RelationalExpressionRecord utc;
  utc.expression_id = 301;
  utc.expression_kind = api::RelationalExpressionKind::kLiteral;
  utc.result_descriptor_id = 302;
  utc.literal_kind = api::RelationalLiteralKind::kTemporal;
  utc.literal_or_parameter_ref = "2026-08-11T20:00:00Z";
  dag.expressions.push_back(std::move(utc));
  api::RelationalExpressionRecord offset;
  offset.expression_id = 302;
  offset.expression_kind = api::RelationalExpressionKind::kLiteral;
  offset.result_descriptor_id = 303;
  offset.literal_kind = api::RelationalLiteralKind::kTemporal;
  offset.literal_or_parameter_ref = "2026-08-11T15:00:00-05:00";
  dag.expressions.push_back(std::move(offset));
  const auto predicate = std::ranges::find_if(
      dag.expressions, [](const auto& expression) {
        return expression.expression_id == 300;
      });
  if (predicate != dag.expressions.end()) {
    predicate->child_expression_ids = {301, 302};
  }
  return dag;
}

api::RelationalTypeDescriptor ProductionDerivedDescriptor(
    const std::uint32_t descriptor_id, const std::string& type_uuid,
    const std::uint64_t salt) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = descriptor_id;
  descriptor.descriptor_uuid =
      ProductionUuid(platform::UuidKind::object, salt);
  descriptor.type_uuid = type_uuid;
  descriptor.nullability = api::RelationalNullability::kNonNull;
  return descriptor;
}

api::TypedRelationalDag ProductionSpatialDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const std::string& crs_uuid, const std::uint64_t salt,
    const std::vector<opt::MultilegDescriptorProfileV1>*
        multileg_profiles = nullptr,
    const std::uint16_t lexical_source_ordinal = 0) {
  const auto boolean_type = ProductionCoreTypeUuid("boolean");
  const auto real64_type = ProductionCoreTypeUuid("real64");
  const auto uint64_type = ProductionCoreTypeUuid("uint64");
  const auto text_type = ProductionCoreTypeUuid("character");
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, salt + 1);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  for (std::size_t ordinal = 0; ordinal < storage.columns.size(); ++ordinal) {
    dag.descriptors.push_back(ProductionDescriptor(
        static_cast<std::uint32_t>(101 + ordinal), storage.columns[ordinal]));
  }
  dag.descriptors.push_back(
      ProductionDerivedDescriptor(104, boolean_type, salt + 2));
  dag.descriptors.push_back(
      ProductionDerivedDescriptor(105, real64_type, salt + 3));
  dag.descriptors.push_back(
      ProductionDerivedDescriptor(106, text_type, salt + 4));
  dag.descriptors.push_back(
      ProductionDerivedDescriptor(107, uint64_type, salt + 5));

  if (multileg_profiles != nullptr) {
    const auto profile = [&](const std::uint8_t kind,
                             const std::uint16_t slot) -> const auto* {
      const auto found = std::ranges::find_if(
          *multileg_profiles, [&](const auto& candidate) {
            return candidate.profile_kind == kind && candidate.slot == slot;
          });
      return found == multileg_profiles->end() ? nullptr : &*found;
    };
    constexpr std::array<std::uint8_t, 5> kKinds{14, 22, 14, 20, 18};
    constexpr std::array<std::uint16_t, 5> kFieldSlots{0, 0, 1, 0, 0};
    for (std::size_t ordinal = 0; ordinal < kKinds.size(); ++ordinal) {
      if (ordinal < storage.columns.size()) continue;
      const auto slot = static_cast<std::uint16_t>(
          kKinds[ordinal] == 14
              ? lexical_source_ordinal * 2 + kFieldSlots[ordinal]
              : lexical_source_ordinal);
      const auto* exact = profile(
          kKinds[ordinal], slot);
      if (exact == nullptr) continue;
      auto& descriptor = dag.descriptors[ordinal];
      descriptor.descriptor_uuid = exact->descriptor_uuid;
      descriptor.type_uuid = exact->type_uuid;
      descriptor.nullability = api::RelationalNullability::kNonNull;
      descriptor.collation_uuid.reset();
      descriptor.timezone_profile_id.reset();
    }
  }

  static constexpr std::array<std::string_view, 5> kOutputNames{
      "row_uuid", "spatial_value", "crs_uuid", "predicate_truth",
      "distance"};
  for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
    api::RelationalExpressionRecord output;
    output.expression_id = static_cast<std::uint32_t>(ordinal + 1);
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    output.bound_name_uuid =
        ordinal < storage.columns.size()
            ? storage.columns[ordinal].column_uuid.canonical
            : storage.relation_uuid.canonical;
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1),
         std::string(kOutputNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord source_root;
  source_root.expression_id = 100;
  source_root.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  source_root.result_descriptor_id = 101;
  source_root.operator_name = "SPATIAL_SOURCE";
  source_root.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(source_root));
  api::RelationalExpressionRecord alias;
  alias.expression_id = 101;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 101;
  alias.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 102;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 106;
  predicate.literal_kind = api::RelationalLiteralKind::kString;
  predicate.literal_or_parameter_ref = "INTERSECTS";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalExpressionRecord x;
  x.expression_id = 103;
  x.expression_kind = api::RelationalExpressionKind::kLiteral;
  x.result_descriptor_id = 105;
  x.literal_kind = api::RelationalLiteralKind::kNumeric;
  x.literal_or_parameter_ref = "0";
  dag.expressions.push_back(x);
  auto y = x;
  y.expression_id = 104;
  dag.expressions.push_back(std::move(y));
  api::RelationalExpressionRecord point;
  point.expression_id = 105;
  point.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  point.child_expression_ids = {103, 104};
  point.result_descriptor_id = 102;
  point.operator_name = "POINT";
  dag.expressions.push_back(std::move(point));
  api::RelationalExpressionRecord crs;
  crs.expression_id = 106;
  crs.expression_kind = api::RelationalExpressionKind::kIdentifier;
  crs.result_descriptor_id = 103;
  crs.bound_name_uuid = crs_uuid;
  dag.expressions.push_back(std::move(crs));
  api::RelationalExpressionRecord match;
  match.expression_id = 107;
  match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  match.child_expression_ids = {101, 102, 105, 106};
  match.result_descriptor_id = 104;
  match.operator_name = "SPATIAL_MATCH";
  dag.expressions.push_back(std::move(match));
  api::RelationalExpressionRecord top_k;
  top_k.expression_id = 108;
  top_k.expression_kind = api::RelationalExpressionKind::kLiteral;
  top_k.result_descriptor_id = 107;
  top_k.literal_kind = api::RelationalLiteralKind::kNumeric;
  top_k.literal_or_parameter_ref = "3";
  dag.expressions.push_back(std::move(top_k));
  api::RelationalExpressionRecord nearest;
  nearest.expression_id = 109;
  nearest.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  nearest.child_expression_ids = {101, 105, 106, 108};
  nearest.result_descriptor_id = 105;
  nearest.operator_name = "SPATIAL_NEAREST";
  dag.expressions.push_back(std::move(nearest));

  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = {101, 102, 103, 104, 105};
  source.bound_expression_ids = {1,   2,   3,   4,   5,   100, 101,
                                 102, 103, 104, 105, 106, 107, 108, 109};
  source.required_object_uuids = {storage.relation_uuid.canonical};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag ProductionSpatialJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& left,
    const api::MgaRelationStorageDescriptor& right,
    const std::string& crs_uuid, const std::uint64_t salt,
    const std::vector<opt::MultilegDescriptorProfileV1>& profiles,
    const std::string_view join_semantic) {
  auto left_dag = ProductionSpatialDag(context, left, crs_uuid, salt + 10,
                                       &profiles, 0);
  auto right_dag = ProductionSpatialDag(context, right, crs_uuid, salt + 20,
                                        &profiles, 1);
  api::TypedRelationalDag dag = left_dag;
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, salt);
  dag.root_node_id = 3;
  auto& left_source = dag.nodes.front();
  left_source.node_id = 1;
  for (auto& output : dag.outputs) output.relation_node_id = 1;

  constexpr std::uint32_t kDescriptorOffset = 1000;
  constexpr std::uint32_t kExpressionOffset = 1000;
  for (auto descriptor : right_dag.descriptors) {
    descriptor.descriptor_id += kDescriptorOffset;
    dag.descriptors.push_back(std::move(descriptor));
  }
  for (auto expression : right_dag.expressions) {
    expression.expression_id += kExpressionOffset;
    expression.result_descriptor_id += kDescriptorOffset;
    for (auto& child : expression.child_expression_ids) {
      child += kExpressionOffset;
    }
    dag.expressions.push_back(std::move(expression));
  }
  for (auto output : right_dag.outputs) {
    output.output_id += 1000;
    output.relation_node_id = 2;
    output.expression_id += kExpressionOffset;
    output.descriptor_id += kDescriptorOffset;
    dag.outputs.push_back(std::move(output));
  }
  auto right_source = right_dag.nodes.front();
  right_source.node_id = 2;
  for (auto& descriptor_id : right_source.output_descriptor_ids) {
    descriptor_id += kDescriptorOffset;
  }
  for (auto& expression_id : right_source.bound_expression_ids) {
    expression_id += kExpressionOffset;
  }
  dag.nodes.push_back(std::move(right_source));

  api::RelationalTypeDescriptor predicate_descriptor;
  predicate_descriptor.descriptor_id = 3001;
  predicate_descriptor.descriptor_uuid =
      ProductionUuid(platform::UuidKind::object, salt + 3);
  predicate_descriptor.type_uuid = ProductionCoreTypeUuid("boolean");
  predicate_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(predicate_descriptor));
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 3000;
  predicate.expression_kind = api::RelationalExpressionKind::kBinary;
  predicate.child_expression_ids = {1, 1001};
  predicate.result_descriptor_id = 3001;
  predicate.operator_name = "=";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = dag.nodes[0].output_descriptor_ids;
  join.output_descriptor_ids.insert(join.output_descriptor_ids.end(),
                                    dag.nodes[1].output_descriptor_ids.begin(),
                                    dag.nodes[1].output_descriptor_ids.end());
  join.bound_expression_ids = {3000};
  join.semantic_variant_id = join_semantic;
  dag.nodes.push_back(std::move(join));
  const auto source_output_count = dag.outputs.size();
  for (std::size_t ordinal = 0; ordinal < source_output_count; ++ordinal) {
    const auto source_output = dag.outputs[ordinal];
    api::RelationalOutputRecord root_output = source_output;
    root_output.output_id =
        static_cast<std::uint32_t>(2001 + ordinal);
    root_output.relation_node_id = 3;
    root_output.ordinal = static_cast<std::uint32_t>(ordinal);
    dag.outputs.push_back(std::move(root_output));
  }
  return dag;
}

api::TypedRelationalDag ProductionSearchDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& storage,
    const std::string& analyzer_uuid, const std::uint64_t salt,
    const std::vector<opt::MultilegDescriptorProfileV1>& profiles) {
  const auto profile = [&](const std::uint8_t kind,
                           const std::uint16_t slot) -> const auto* {
    const auto found = std::ranges::find_if(
        profiles, [&](const auto& candidate) {
          return candidate.profile_kind == kind && candidate.slot == slot;
        });
    return found == profiles.end() ? nullptr : &*found;
  };
  constexpr std::array<std::uint8_t, 5> kKinds{14, 14, 16, 18, 16};
  // The preceding spatial leg consumes UUID slots 0/1 and REAL64 slot 0.
  constexpr std::array<std::uint16_t, 5> kSlots{2, 3, 0, 1, 1};
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, salt);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  for (std::size_t ordinal = 0; ordinal < kKinds.size(); ++ordinal) {
    const auto* exact = profile(kKinds[ordinal], kSlots[ordinal]);
    if (exact == nullptr) continue;
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    descriptor.descriptor_uuid = exact->descriptor_uuid;
    descriptor.type_uuid = exact->type_uuid;
    descriptor.nullability = api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(descriptor));
  }
  dag.descriptors.push_back(ProductionDescriptor(106, storage.columns[0]));
  dag.descriptors.push_back(ProductionDescriptor(107, storage.columns[1]));

  static constexpr std::array<std::string_view, 5> kNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    api::RelationalExpressionRecord output;
    output.expression_id = static_cast<std::uint32_t>(ordinal + 1);
    output.result_descriptor_id = static_cast<std::uint32_t>(101 + ordinal);
    if (ordinal == 1) {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid = analyzer_uuid;
    } else if (ordinal == 2) {
      output.expression_kind = api::RelationalExpressionKind::kLiteral;
      output.literal_kind = api::RelationalLiteralKind::kNumeric;
      output.literal_or_parameter_ref = "7";
    } else if (ordinal == 4) {
      output.expression_kind = api::RelationalExpressionKind::kLiteral;
      output.literal_kind = api::RelationalLiteralKind::kNumeric;
      output.literal_or_parameter_ref = "2";
    } else {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid = storage.relation_uuid.canonical;
    }
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1), std::string(kNames[ordinal]),
         static_cast<std::uint32_t>(101 + ordinal), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord alias;
  alias.expression_id = 6;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 106;
  alias.bound_name_uuid = storage.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord text;
  text.expression_id = 7;
  text.expression_kind = api::RelationalExpressionKind::kLiteral;
  text.result_descriptor_id = 106;
  text.literal_kind = api::RelationalLiteralKind::kString;
  text.literal_or_parameter_ref = "alpha";
  dag.expressions.push_back(std::move(text));
  api::RelationalExpressionRecord query;
  query.expression_id = 8;
  query.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  query.result_descriptor_id = 106;
  query.operator_name = "SEARCH_TERMS";
  query.child_expression_ids = {7};
  dag.expressions.push_back(std::move(query));
  api::RelationalExpressionRecord digest;
  digest.expression_id = 9;
  digest.expression_kind = api::RelationalExpressionKind::kLiteral;
  digest.result_descriptor_id = 106;
  digest.literal_kind = api::RelationalLiteralKind::kString;
  digest.literal_or_parameter_ref =
      "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";
  dag.expressions.push_back(std::move(digest));
  api::RelationalExpressionRecord binding;
  binding.expression_id = 10;
  binding.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  binding.result_descriptor_id = 102;
  binding.operator_name = "SEARCH_ANALYZER_BINDING";
  binding.child_expression_ids = {2, 3, 9};
  dag.expressions.push_back(std::move(binding));
  api::RelationalExpressionRecord match;
  match.expression_id = 11;
  match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  match.result_descriptor_id = 101;
  match.operator_name = "SEARCH_MATCH";
  match.child_expression_ids = {6, 8, 10, 5};
  dag.expressions.push_back(std::move(match));
  api::RelationalExpressionRecord category;
  category.expression_id = 12;
  category.expression_kind = api::RelationalExpressionKind::kIdentifier;
  category.result_descriptor_id = 107;
  category.bound_name_uuid = storage.columns[1].column_uuid.canonical;
  dag.expressions.push_back(std::move(category));

  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.output_descriptor_ids = {101, 102, 103, 104, 105};
  for (const auto& expression : dag.expressions) {
    source.bound_expression_ids.push_back(expression.expression_id);
  }
  source.required_object_uuids = {storage.relation_uuid.canonical};
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(source));
  return dag;
}

api::TypedRelationalDag ProductionSpatialSearchJoinDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& spatial,
    const api::MgaRelationStorageDescriptor& search,
    const std::string& crs_uuid, const std::string& analyzer_uuid,
    const std::uint64_t salt,
    const std::vector<opt::MultilegDescriptorProfileV1>& profiles,
    const std::string_view join_semantic) {
  auto dag = ProductionSpatialDag(context, spatial, crs_uuid, salt + 10,
                                  &profiles, 0);
  auto search_dag =
      ProductionSearchDag(context, search, analyzer_uuid, salt + 20, profiles);
  dag.bound_sblr_tree_uuid =
      ProductionUuid(platform::UuidKind::object, salt);
  dag.root_node_id = 3;
  for (auto& output : dag.outputs) output.relation_node_id = 1;

  constexpr std::uint32_t kDescriptorOffset = 1000;
  constexpr std::uint32_t kExpressionOffset = 1000;
  for (auto descriptor : search_dag.descriptors) {
    descriptor.descriptor_id += kDescriptorOffset;
    dag.descriptors.push_back(std::move(descriptor));
  }
  for (auto expression : search_dag.expressions) {
    expression.expression_id += kExpressionOffset;
    expression.result_descriptor_id += kDescriptorOffset;
    for (auto& child : expression.child_expression_ids) {
      child += kExpressionOffset;
    }
    dag.expressions.push_back(std::move(expression));
  }
  for (auto output : search_dag.outputs) {
    output.output_id += kExpressionOffset;
    output.relation_node_id = 2;
    output.expression_id += kExpressionOffset;
    output.descriptor_id += kDescriptorOffset;
    dag.outputs.push_back(std::move(output));
  }
  auto search_source = search_dag.nodes.front();
  search_source.node_id = 2;
  for (auto& descriptor_id : search_source.output_descriptor_ids) {
    descriptor_id += kDescriptorOffset;
  }
  for (auto& expression_id : search_source.bound_expression_ids) {
    expression_id += kExpressionOffset;
  }
  dag.nodes.push_back(std::move(search_source));

  api::RelationalTypeDescriptor predicate_descriptor;
  predicate_descriptor.descriptor_id = 3001;
  predicate_descriptor.descriptor_uuid =
      ProductionUuid(platform::UuidKind::object, salt + 3);
  predicate_descriptor.type_uuid = ProductionCoreTypeUuid("boolean");
  predicate_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(predicate_descriptor));
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 3000;
  predicate.expression_kind = api::RelationalExpressionKind::kBinary;
  predicate.child_expression_ids = {1, 1001};
  predicate.result_descriptor_id = 3001;
  predicate.operator_name = "=";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = dag.nodes[0].output_descriptor_ids;
  join.output_descriptor_ids.insert(join.output_descriptor_ids.end(),
                                    dag.nodes[1].output_descriptor_ids.begin(),
                                    dag.nodes[1].output_descriptor_ids.end());
  join.bound_expression_ids = {3000};
  join.semantic_variant_id = join_semantic;
  dag.nodes.push_back(std::move(join));
  const auto source_output_count = dag.outputs.size();
  for (std::size_t ordinal = 0; ordinal < source_output_count; ++ordinal) {
    auto root_output = dag.outputs[ordinal];
    root_output.output_id = static_cast<std::uint32_t>(2001 + ordinal);
    root_output.relation_node_id = 3;
    root_output.ordinal = static_cast<std::uint32_t>(ordinal);
    dag.outputs.push_back(std::move(root_output));
  }
  return dag;
}

bool ProductionSpatialRoute() {
  ProductionFixture fixture;
  fixture.salt = (ProductionNowMillis() % 1'000'000) + 1000;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_rcp079_spatial_production_" +
                       std::to_string(fixture.salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture.directory, filesystem_error);
  if (!Require(!filesystem_error,
               "production spatial fixture directory creation failed")) {
    return false;
  }
  fixture.database_path = fixture.directory / "spatial.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, ProductionNowMillis() + fixture.salt + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, ProductionNowMillis() + fixture.salt + 2);
  if (!Require(database_uuid.ok() && filespace_uuid.ok(),
               "production spatial database identity creation failed")) {
    return false;
  }
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = ProductionNowMillis();
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!Require(created.ok(),
               "production spatial database creation failed:" +
                   created.diagnostic.diagnostic_code)) {
    return false;
  }
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture.schema_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 10);
  fixture.principal_uuid =
      ProductionUuid(platform::UuidKind::principal, fixture.salt + 11);
  fixture.session_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 12);
  fixture.relation_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 13);
  const auto crs_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 14);
  const auto right_relation_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 15);
  const auto search_relation_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 16);
  const auto analyzer_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 17);
  const auto uuid_type = ProductionCoreTypeUuid("uuid");
  const auto geometry_type = ProductionCoreTypeUuid("geometry");
  const auto text_type = ProductionCoreTypeUuid("character");
  if (!Require(!uuid_type.empty() && !geometry_type.empty() &&
                   !text_type.empty(),
               "production spatial core type UUIDs are unavailable")) {
    return false;
  }

  api::EngineRequestContext metadata;
  if (!Require(ProductionBegin(fixture, "rcp079-spatial-metadata", &metadata),
               "production spatial metadata transaction failed")) {
    return false;
  }
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.relation_uuid;
  table.default_name = "rcp079_spatial_production";
  table.columns = {
      {"row_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                       ";nullable=false"},
      {"spatial_value", "canonical=geometry;type_uuid=" + geometry_type +
                            ";nullable=false;subtype=POINT;axes=x,y;" +
                            "crs_uuid=" + crs_uuid + ";crs_generation=1"},
      {"crs_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                      ";nullable=false"},
  };
  api::MgaRelationStorageDescriptor storage;
  auto right_table = table;
  right_table.table_uuid = right_relation_uuid;
  right_table.default_name = "rcp079_spatial_production_right";
  api::MgaRelationStorageDescriptor right_storage;
  api::CrudTableRecord search_table;
  search_table.creator_tx = metadata.local_transaction_id;
  search_table.table_uuid = search_relation_uuid;
  search_table.default_name = "rcp079_spatial_search_production";
  search_table.columns = {
      {"body", "canonical=text;type_uuid=" + text_type + ";nullable=false"},
      {"category",
       "canonical=text;type_uuid=" + text_type + ";nullable=false"},
  };
  api::MgaRelationStorageDescriptor search_storage;
  if (!Require(!api::AppendMgaTableMetadata(metadata, table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(metadata, table, {},
                                                            &storage)
                        .error &&
                   !api::AppendMgaTableMetadata(metadata, right_table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(
                        metadata, right_table, {}, &right_storage)
                        .error &&
                   !api::AppendMgaTableMetadata(metadata, search_table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(
                        metadata, search_table, {}, &search_storage)
                        .error &&
                   ProductionCommit(metadata),
               "production spatial storage descriptor persistence failed")) {
    return false;
  }

  api::EngineRequestContext writer;
  if (!Require(ProductionBegin(fixture, "rcp079-spatial-writer", &writer),
               "production spatial writer transaction failed")) {
    return false;
  }
  struct Seed {
    std::string row_uuid;
    nosql::SpatialPoint2dV1 point;
  };
  const std::array<Seed, 3> seeds{{
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 20), {0, 0}},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 21), {3, 4}},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 22), {0, 0}},
  }};
  for (std::size_t ordinal = 0; ordinal < seeds.size(); ++ordinal) {
    const auto encoded = nosql::EncodeSpatialPoint2dV1(seeds[ordinal].point);
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = fixture.relation_uuid;
    row.row_uuid = seeds[ordinal].row_uuid;
    row.version_uuid =
        ProductionUuid(platform::UuidKind::object, fixture.salt + 30 + ordinal);
    row.values = {
        {"row_uuid", seeds[ordinal].row_uuid},
        {"spatial_value",
         std::string(reinterpret_cast<const char*>(encoded.data()),
                     encoded.size())},
        {"crs_uuid", crs_uuid},
    };
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production spatial row persistence failed")) {
      return false;
    }
  }
  const std::array<Seed, 2> right_seeds{{
      {seeds[0].row_uuid, {0, 0}},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 50), {0, 0}},
  }};
  for (std::size_t ordinal = 0; ordinal < right_seeds.size(); ++ordinal) {
    const auto encoded =
        nosql::EncodeSpatialPoint2dV1(right_seeds[ordinal].point);
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = right_relation_uuid;
    row.row_uuid = right_seeds[ordinal].row_uuid;
    row.version_uuid =
        ProductionUuid(platform::UuidKind::object, fixture.salt + 60 + ordinal);
    row.values = {
        {"row_uuid", right_seeds[ordinal].row_uuid},
        {"spatial_value",
         std::string(reinterpret_cast<const char*>(encoded.data()),
                     encoded.size())},
        {"crs_uuid", crs_uuid},
    };
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production right spatial row persistence failed")) {
      return false;
    }
  }
  const std::array<Seed, 2> search_seeds{{
      {seeds[0].row_uuid, {0, 0}},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 70), {0, 0}},
  }};
  for (std::size_t ordinal = 0; ordinal < search_seeds.size(); ++ordinal) {
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = search_relation_uuid;
    row.row_uuid = search_seeds[ordinal].row_uuid;
    row.version_uuid =
        ProductionUuid(platform::UuidKind::object, fixture.salt + 80 + ordinal);
    row.values = {{"body", "alpha " + std::to_string(ordinal)},
                  {"category", ordinal == 0 ? "matched" : "unmatched"}};
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production search row persistence failed")) {
      return false;
    }
  }
  if (!Require(ProductionCommit(writer),
               "production spatial writer commit failed")) {
    return false;
  }

  api::EngineRequestContext reader;
  if (!Require(ProductionBegin(fixture, "rcp079-spatial-reader", &reader) &&
                   ProductionPublishSnapshot(&reader, fixture.salt + 40),
               "production spatial reader snapshot failed")) {
    return false;
  }
  reader.statement_timestamp = "2026-08-11T20:00:00Z";
  reader.statement_metadata_snapshot_engine_owned = true;
  reader.statement_metadata_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 41);
  reader.statement_metadata_snapshot_visible_through_local_transaction_id =
      reader.snapshot_visible_through_local_transaction_id;
  reader.catalog_epoch_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 42);
  reader.optimizer_capability_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 43);
  reader.optimizer_resource_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 44);
  reader.optimizer_route_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 45);
  reader.optimizer_route_epoch = 1;
  reader.optimizer_route_generation = 1;
  reader.optimizer_memory_budget_bytes = 16 * 1024 * 1024;
  reader.optimizer_maximum_candidate_count = 4096;
  reader.optimizer_maximum_memo_groups = 4096;
  reader.optimizer_maximum_search_steps = 16384;
  reader.optimizer_maximum_planning_time_ns = 1'000'000'000;
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis());
  reader.query_cancellation_requested = [] { return false; };
  AddProductionAuthorization(&reader, fixture.relation_uuid);
  AddProductionAuthorization(&reader, right_relation_uuid);
  AddProductionAuthorization(&reader, search_relation_uuid);
  const auto execution = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader,
       ProductionSpatialDag(reader, storage, crs_uuid, fixture.salt + 100)});
  const auto diagnostic = execution.api_result.diagnostics.empty()
                              ? std::string{}
                              : execution.api_result.diagnostics.front().code +
                                    ":" + execution.api_result.diagnostics.front().detail;
  const bool exact =
      execution.profile_matched && execution.optimizer_admitted &&
      execution.optimizer_selected && execution.physical_dag_published &&
      execution.physical_dag_executed && execution.runtime_actuals_attached &&
      execution.canonical_result_published && execution.api_result.ok &&
      execution.physical_node_count == 1 &&
      execution.canonical_result_column_count == 5 &&
      execution.canonical_result_row_count == 2 &&
      execution.api_result.result_shape.rows.size() == 2 &&
      execution.api_result.result_shape.rows[0].fields.size() == 5 &&
      execution.api_result.result_shape.rows[0].fields[3].second.encoded_value ==
          "true" &&
      execution.api_result.result_shape.rows[0].fields[4].second.encoded_value ==
          "0" &&
      execution.api_result.result_shape.rows[1].fields[4].second.encoded_value ==
          "0";
  auto profiles = ProductionMultilegProfiles(fixture.salt + 80'000);
  const auto bind_persisted_profile =
      [&](const std::uint8_t kind, const std::uint16_t slot,
          const api::MgaRelationColumnStorageDescriptor& column) {
        const auto found = std::ranges::find_if(
            profiles, [&](const auto& candidate) {
              return candidate.profile_kind == kind &&
                     candidate.slot == slot;
            });
        if (found == profiles.end()) return false;
        found->descriptor_uuid =
            column.value_descriptor.descriptor_uuid.canonical;
        found->type_uuid = DescriptorTypeUuid(column.value_descriptor);
        return !found->descriptor_uuid.empty() && !found->type_uuid.empty();
      };
  if (!Require(
          bind_persisted_profile(14, 0, storage.columns[0]) &&
              bind_persisted_profile(22, 0, storage.columns[1]) &&
              bind_persisted_profile(14, 1, storage.columns[2]) &&
              bind_persisted_profile(14, 2, right_storage.columns[0]) &&
              bind_persisted_profile(22, 1, right_storage.columns[1]) &&
              bind_persisted_profile(14, 3, right_storage.columns[2]),
          "spatial multileg base descriptors were not persisted-exact")) {
    return false;
  }
  const auto profile = [&](const std::uint8_t kind,
                           const std::uint16_t slot) -> const auto* {
    const auto found = std::ranges::find_if(
        profiles, [&](const auto& candidate) {
          return candidate.profile_kind == kind && candidate.slot == slot;
        });
    return found == profiles.end() ? nullptr : &*found;
  };
  const auto source_binding_exact = [&](const api::TypedRelationalDag& dag) {
    constexpr std::array<std::uint8_t, 5> kKinds{14, 22, 14, 20, 18};
    constexpr std::array<std::uint16_t, 5> kUuidFieldSlots{0, 0, 1, 0, 0};
    for (std::size_t source_ordinal = 0; source_ordinal < 2;
         ++source_ordinal) {
      for (std::size_t field_ordinal = 0; field_ordinal < kKinds.size();
           ++field_ordinal) {
        const auto descriptor_id = static_cast<std::uint32_t>(
            101 + field_ordinal + source_ordinal * 1000);
        const auto descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return candidate.descriptor_id == descriptor_id;
            });
        const auto slot = static_cast<std::uint16_t>(
            kKinds[field_ordinal] == 14
                ? source_ordinal * 2 + kUuidFieldSlots[field_ordinal]
                : source_ordinal);
        const auto* expected = profile(kKinds[field_ordinal], slot);
        const auto& persisted =
            source_ordinal == 0 ? storage : right_storage;
        const auto expected_descriptor_uuid =
            field_ordinal < persisted.columns.size()
                ? persisted.columns[field_ordinal]
                      .value_descriptor.descriptor_uuid.canonical
                : expected == nullptr ? std::string{}
                                      : expected->descriptor_uuid;
        const auto expected_type_uuid =
            field_ordinal < persisted.columns.size()
                ? DescriptorTypeUuid(
                      persisted.columns[field_ordinal].value_descriptor)
                : expected == nullptr ? std::string{} : expected->type_uuid;
        if (descriptor == dag.descriptors.end() ||
            expected_descriptor_uuid.empty() || expected_type_uuid.empty() ||
            descriptor->descriptor_uuid != expected_descriptor_uuid ||
            descriptor->type_uuid != expected_type_uuid ||
            descriptor->nullability !=
                api::RelationalNullability::kNonNull) {
          return false;
        }
      }
    }
    return true;
  };
  const auto execute_outer = [&](const std::string_view semantic,
                                 const std::uint64_t salt) {
    auto dag = ProductionSpatialJoinDag(reader, storage, right_storage,
                                        crs_uuid, salt, profiles, semantic);
    const bool exact_source_binding = source_binding_exact(dag);
    reader.current_monotonic_ns =
        std::to_string(ProductionNowMillis() + salt % 1000);
    return std::pair{
        exact_source_binding,
        sblr::ExecuteCanonicalCurrentHeapQuery({reader, std::move(dag)})};
  };
  const auto outer_exact = [&](const auto& execution,
                               const std::size_t expected_rows,
                               const bool left_nullable,
                               const bool right_nullable) {
    if (!execution.profile_matched || !execution.optimizer_admitted ||
        !execution.optimizer_selected || !execution.physical_dag_published ||
        !execution.physical_dag_executed ||
        !execution.runtime_actuals_attached ||
        !execution.canonical_result_published || !execution.api_result.ok ||
        execution.physical_node_count != 3 ||
        execution.canonical_result_column_count != 10 ||
        execution.canonical_result_row_count != expected_rows ||
        execution.api_result.result_shape.columns.size() != 10 ||
        execution.api_result.result_shape.rows.size() != expected_rows) {
      return false;
    }
    constexpr std::array<std::uint8_t, 5> kKinds{14, 22, 14, 20, 18};
    constexpr std::array<std::uint16_t, 5> kUuidFieldSlots{0, 0, 1, 0, 0};
    for (std::size_t ordinal = 0; ordinal < 10; ++ordinal) {
      const auto source_ordinal = ordinal / 5;
      const auto field_ordinal = ordinal % 5;
      const bool nullable = source_ordinal == 0 ? left_nullable
                                                : right_nullable;
      const auto kind = static_cast<std::uint8_t>(
          kKinds[field_ordinal] + (nullable ? 1 : 0));
      std::size_t pool_source_ordinal = source_ordinal;
      if (nullable && source_ordinal == 1 && !left_nullable) {
        pool_source_ordinal = 0;
      }
      const auto slot = static_cast<std::uint16_t>(
          kKinds[field_ordinal] == 14
              ? pool_source_ordinal * 2 + kUuidFieldSlots[field_ordinal]
              : pool_source_ordinal);
      const auto* expected = profile(kind, slot);
      const auto& column = execution.api_result.result_shape.columns[ordinal];
      if (expected == nullptr ||
          column.descriptor_uuid.canonical != expected->descriptor_uuid ||
          DescriptorTypeUuid(column) != expected->type_uuid ||
          column.encoded_descriptor.find(
              nullable ? "nullability=nullable"
                       : "nullability=non_null") == std::string::npos) {
        return false;
      }
      for (const auto& row : execution.api_result.result_shape.rows) {
        if (row.fields.size() != 10 ||
            row.fields[ordinal].second.descriptor.descriptor_uuid.canonical !=
                expected->descriptor_uuid) {
          return false;
        }
      }
    }
    const bool has_left_null = std::ranges::any_of(
        execution.api_result.result_shape.rows, [](const auto& row) {
          return row.fields[0].second.isSqlNull();
        });
    const bool has_right_null = std::ranges::any_of(
        execution.api_result.result_shape.rows, [](const auto& row) {
          return row.fields[5].second.isSqlNull();
        });
    return has_left_null == left_nullable &&
           has_right_null == right_nullable;
  };
  bool left_outer_exact = false;
  bool right_outer_exact = false;
  bool full_outer_exact = false;
  std::string outer_diagnostic;
  {
    opt::MultilegDescriptorDispatchScopeV1 descriptor_scope(
        reader.statement_uuid.canonical, profiles);
    const auto left_outer = execute_outer(
        "join.left-outer.on.v1", fixture.salt + 200);
    const auto right_outer = execute_outer(
        "join.right-outer.on.v1", fixture.salt + 300);
    const auto full_outer = execute_outer(
        "join.full-outer.on.v1", fixture.salt + 400);
    left_outer_exact = descriptor_scope.installed() && left_outer.first &&
                       outer_exact(left_outer.second, 2, false, true);
    right_outer_exact = right_outer.first &&
                        outer_exact(right_outer.second, 2, true, false);
    full_outer_exact = full_outer.first &&
                       outer_exact(full_outer.second, 3, true, true);
    for (const auto* candidate :
         {&left_outer.second, &right_outer.second, &full_outer.second}) {
      if (!candidate->api_result.diagnostics.empty()) {
        outer_diagnostic += candidate->api_result.diagnostics.front().code +
                            ":" +
                            candidate->api_result.diagnostics.front().detail +
                            ";";
      }
    }
  }
  const auto mixed_source_binding_exact =
      [&](const api::TypedRelationalDag& dag) {
        constexpr std::array<std::uint8_t, 10> kKinds{
            14, 22, 14, 20, 18, 14, 14, 16, 18, 16};
        constexpr std::array<std::uint16_t, 10> kSlots{
            0, 0, 1, 0, 0, 2, 3, 0, 1, 1};
        for (std::size_t ordinal = 0; ordinal < kKinds.size(); ++ordinal) {
          const auto descriptor_id = static_cast<std::uint32_t>(
              101 + ordinal % 5 + (ordinal / 5) * 1000);
          const auto descriptor = std::ranges::find_if(
              dag.descriptors, [&](const auto& candidate) {
                return candidate.descriptor_id == descriptor_id;
              });
          const auto* expected = profile(kKinds[ordinal], kSlots[ordinal]);
          const auto spatial_base = ordinal < storage.columns.size();
          const auto expected_descriptor_uuid =
              spatial_base
                  ? storage.columns[ordinal]
                        .value_descriptor.descriptor_uuid.canonical
                  : expected == nullptr ? std::string{}
                                        : expected->descriptor_uuid;
          const auto expected_type_uuid =
              spatial_base
                  ? DescriptorTypeUuid(
                        storage.columns[ordinal].value_descriptor)
                  : expected == nullptr ? std::string{} : expected->type_uuid;
          if (descriptor == dag.descriptors.end() ||
              expected_descriptor_uuid.empty() ||
              expected_type_uuid.empty() ||
              descriptor->descriptor_uuid != expected_descriptor_uuid ||
              descriptor->type_uuid != expected_type_uuid ||
              descriptor->nullability !=
                  api::RelationalNullability::kNonNull) {
            return false;
          }
        }
        return true;
      };
  const auto execute_mixed_outer = [&](const std::string_view semantic,
                                       const std::uint64_t salt) {
    auto dag = ProductionSpatialSearchJoinDag(
        reader, storage, search_storage, crs_uuid, analyzer_uuid, salt,
        profiles, semantic);
    const bool exact_source_binding = mixed_source_binding_exact(dag);
    reader.current_monotonic_ns =
        std::to_string(ProductionNowMillis() + salt % 1000);
    return std::pair{
        exact_source_binding,
        sblr::ExecuteCanonicalCurrentHeapQuery({reader, std::move(dag)})};
  };
  const auto mixed_outer_exact = [&](const auto& execution,
                                     const std::size_t expected_rows,
                                     const bool left_nullable,
                                     const bool right_nullable) {
    if (!execution.profile_matched || !execution.optimizer_admitted ||
        !execution.optimizer_selected || !execution.physical_dag_published ||
        !execution.physical_dag_executed ||
        !execution.runtime_actuals_attached ||
        !execution.canonical_result_published || !execution.api_result.ok ||
        execution.physical_node_count != 3 ||
        execution.canonical_result_column_count != 10 ||
        execution.canonical_result_row_count != expected_rows ||
        execution.api_result.result_shape.columns.size() != 10 ||
        execution.api_result.result_shape.rows.size() != expected_rows) {
      return false;
    }
    constexpr std::array<std::uint8_t, 10> kKinds{
        14, 22, 14, 20, 18, 14, 14, 16, 18, 16};
    constexpr std::array<std::uint16_t, 10> kSourceOrFullSlots{
        0, 0, 1, 0, 0, 2, 3, 0, 1, 1};
    constexpr std::array<std::uint16_t, 5> kRightOnlyNullableSlots{
        0, 1, 0, 0, 1};
    for (std::size_t ordinal = 0; ordinal < kKinds.size(); ++ordinal) {
      const bool nullable = ordinal < 5 ? left_nullable : right_nullable;
      auto slot = kSourceOrFullSlots[ordinal];
      if (nullable && ordinal >= 5 && !left_nullable) {
        slot = kRightOnlyNullableSlots[ordinal - 5];
      }
      const auto kind =
          static_cast<std::uint8_t>(kKinds[ordinal] + (nullable ? 1 : 0));
      const auto* expected = profile(kind, slot);
      const auto& column = execution.api_result.result_shape.columns[ordinal];
      if (expected == nullptr ||
          column.descriptor_uuid.canonical != expected->descriptor_uuid ||
          DescriptorTypeUuid(column) != expected->type_uuid ||
          column.encoded_descriptor.find(
              nullable ? "nullability=nullable"
                       : "nullability=non_null") == std::string::npos) {
        return false;
      }
      for (const auto& row : execution.api_result.result_shape.rows) {
        if (row.fields.size() != 10 ||
            row.fields[ordinal].second.descriptor.descriptor_uuid.canonical !=
                expected->descriptor_uuid) {
          return false;
        }
      }
    }
    const bool has_left_null = std::ranges::any_of(
        execution.api_result.result_shape.rows, [](const auto& row) {
          return row.fields[0].second.isSqlNull();
        });
    const bool has_right_null = std::ranges::any_of(
        execution.api_result.result_shape.rows, [](const auto& row) {
          return row.fields[5].second.isSqlNull();
        });
    const bool has_match = std::ranges::any_of(
        execution.api_result.result_shape.rows, [&](const auto& row) {
          return !row.fields[0].second.isSqlNull() &&
                 !row.fields[5].second.isSqlNull() &&
                 row.fields[0].second.encoded_value == seeds[0].row_uuid &&
                 row.fields[5].second.encoded_value == seeds[0].row_uuid;
        });
    return has_left_null == left_nullable &&
           has_right_null == right_nullable && has_match &&
           HasEvidence(execution, "canonical.model_join_left_provider_route",
                       "canonical.model-provider.spatial.v1") &&
           HasEvidence(execution, "canonical.model_join_right_provider_route",
                       "canonical.model-provider.search.v1") &&
           HasEvidence(execution, "canonical.model_join_consumer_route",
                       "canonical.relational.join-3vl-nested.v1");
  };
  bool mixed_left_exact = false;
  bool mixed_right_exact = false;
  bool mixed_full_exact = false;
  std::string mixed_diagnostic;
  {
    opt::MultilegDescriptorDispatchScopeV1 descriptor_scope(
        reader.statement_uuid.canonical, profiles);
    const auto left_outer = execute_mixed_outer(
        "join.left-outer.on.v1", fixture.salt + 500);
    const auto right_outer = execute_mixed_outer(
        "join.right-outer.on.v1", fixture.salt + 600);
    const auto full_outer = execute_mixed_outer(
        "join.full-outer.on.v1", fixture.salt + 700);
    mixed_left_exact = descriptor_scope.installed() && left_outer.first &&
                       mixed_outer_exact(left_outer.second, 2, false, true);
    mixed_right_exact = right_outer.first &&
                        mixed_outer_exact(right_outer.second, 2, true, false);
    mixed_full_exact = full_outer.first &&
                       mixed_outer_exact(full_outer.second, 3, true, true);
    for (const auto* candidate :
         {&left_outer.second, &right_outer.second, &full_outer.second}) {
      if (!candidate->api_result.diagnostics.empty()) {
        mixed_diagnostic += candidate->api_result.diagnostics.front().code +
                            ":" +
                            candidate->api_result.diagnostics.front().detail +
                            ";";
      }
    }
  }
  const bool rolled_back = ProductionRollback(reader);
  return Require(exact,
                 "production canonical spatial route drifted: " + diagnostic) &&
         Require(left_outer_exact,
                 "supplementary spatial/spatial LEFT descriptor route "
                 "drifted: " + outer_diagnostic) &&
         Require(right_outer_exact,
                 "supplementary spatial/spatial RIGHT descriptor route "
                 "drifted: " + outer_diagnostic) &&
         Require(full_outer_exact,
                 "supplementary spatial/spatial FULL descriptor route "
                 "drifted: " + outer_diagnostic) &&
         Require(mixed_left_exact,
                 "SPATIAL-DV006 spatial/search LEFT descriptor route "
                 "drifted: " + mixed_diagnostic) &&
         Require(mixed_right_exact,
                 "SPATIAL-DV006 spatial/search RIGHT descriptor route "
                 "drifted: " + mixed_diagnostic) &&
         Require(mixed_full_exact,
                 "SPATIAL-DV006 spatial/search FULL descriptor route "
                 "drifted: " + mixed_diagnostic) &&
         Require(rolled_back, "production spatial reader rollback failed");
}

bool ProductionColumnarRoute() {
  ProductionFixture fixture;
  fixture.salt = ProductionNowMillis() % 1'000'000;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_rcp079_columnar_production_" +
                       std::to_string(fixture.salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture.directory, filesystem_error);
  if (!Require(!filesystem_error,
               "production columnar fixture directory creation failed")) {
    return false;
  }
  fixture.database_path = fixture.directory / "columnar.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, ProductionNowMillis() + fixture.salt + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, ProductionNowMillis() + fixture.salt + 2);
  if (!Require(database_uuid.ok() && filespace_uuid.ok(),
               "production columnar database identity creation failed")) {
    return false;
  }
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = ProductionNowMillis();
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!Require(created.ok(),
               "production columnar database creation failed:" +
                   created.diagnostic.diagnostic_code)) {
    return false;
  }
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture.schema_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 10);
  fixture.principal_uuid =
      ProductionUuid(platform::UuidKind::principal, fixture.salt + 11);
  fixture.session_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 12);
  fixture.relation_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 13);
  const auto right_relation_uuid =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 14);
  const auto uuid_type = ProductionCoreTypeUuid("uuid");
  const auto int64_type = ProductionCoreTypeUuid("int64");
  const auto text_type = ProductionCoreTypeUuid("character");
  if (!Require(!uuid_type.empty() && !int64_type.empty() &&
                   !text_type.empty(),
               "production columnar core type UUIDs are unavailable")) {
    return false;
  }

  api::EngineRequestContext metadata;
  if (!Require(ProductionBegin(fixture, "rcp079-columnar-metadata", &metadata),
               "production columnar metadata transaction failed")) {
    return false;
  }
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = fixture.relation_uuid;
  table.default_name = "rcp079_columnar_production";
  table.columns = {
      {"row_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                       ";nullable=false"},
      {"join_key", "canonical=int64;type_uuid=" + int64_type +
                       ";nullable=false"},
      {"payload", "canonical=character;type_uuid=" + text_type +
                      ";nullable=true"},
  };
  api::MgaRelationStorageDescriptor storage;
  auto right_table = table;
  right_table.table_uuid = right_relation_uuid;
  right_table.default_name = "rcp079_columnar_production_right";
  for (auto& column : right_table.columns) {
    const auto non_null = column.second.find("nullable=false");
    if (non_null != std::string::npos) {
      column.second.replace(non_null, std::string("nullable=false").size(),
                            "nullable=true");
    }
  }
  api::MgaRelationStorageDescriptor right_storage;
  if (!Require(!api::AppendMgaTableMetadata(metadata, table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(metadata, table, {},
                                                            &storage)
                        .error &&
                   !api::AppendMgaTableMetadata(metadata, right_table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(
                        metadata, right_table, {}, &right_storage).error &&
                   ProductionCommit(metadata),
               "production columnar storage descriptor persistence failed")) {
    return false;
  }

  api::EngineRequestContext writer;
  if (!Require(ProductionBegin(fixture, "rcp079-columnar-writer", &writer),
               "production columnar writer transaction failed")) {
    return false;
  }
  struct Seed {
    std::string row_uuid;
    std::string join_key;
    std::string payload;
  };
  const std::array<Seed, 3> seeds{{
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 20), "1",
       "alpha"},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 21), "2",
       "<NULL>"},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 22), "3",
       "beta"},
  }};
  for (std::size_t ordinal = 0; ordinal < seeds.size(); ++ordinal) {
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = fixture.relation_uuid;
    row.row_uuid = seeds[ordinal].row_uuid;
    row.version_uuid =
        ProductionUuid(platform::UuidKind::object, fixture.salt + 30 + ordinal);
    row.values = {{"row_uuid", seeds[ordinal].row_uuid},
                  {"join_key", seeds[ordinal].join_key},
                  {"payload", seeds[ordinal].payload}};
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production columnar row persistence failed")) {
      return false;
    }
  }
  const std::array<Seed, 3> right_seeds{{
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 50), "2",
       "right-two"},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 51), "3",
       "right-three"},
      {ProductionUuid(platform::UuidKind::row, fixture.salt + 52), "4",
       "right-four"},
  }};
  for (std::size_t ordinal = 0; ordinal < right_seeds.size(); ++ordinal) {
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = right_relation_uuid;
    row.row_uuid = right_seeds[ordinal].row_uuid;
    row.version_uuid =
        ProductionUuid(platform::UuidKind::object, fixture.salt + 60 + ordinal);
    row.values = {{"row_uuid", right_seeds[ordinal].row_uuid},
                  {"join_key", right_seeds[ordinal].join_key},
                  {"payload", right_seeds[ordinal].payload}};
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production right columnar row persistence failed")) {
      return false;
    }
  }
  if (!Require(ProductionCommit(writer),
               "production columnar writer commit failed")) {
    return false;
  }

  api::EngineRequestContext reader;
  if (!Require(ProductionBegin(fixture, "rcp079-columnar-reader", &reader) &&
                   ProductionPublishSnapshot(&reader, fixture.salt + 40),
               "production columnar reader snapshot failed")) {
    return false;
  }
  reader.statement_timestamp = "2026-08-11T20:00:00Z";
  reader.statement_metadata_snapshot_engine_owned = true;
  reader.statement_metadata_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 41);
  reader.statement_metadata_snapshot_visible_through_local_transaction_id =
      reader.snapshot_visible_through_local_transaction_id;
  reader.catalog_epoch_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 42);
  reader.optimizer_capability_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 43);
  reader.optimizer_resource_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 44);
  reader.optimizer_route_snapshot_uuid.canonical =
      ProductionUuid(platform::UuidKind::object, fixture.salt + 45);
  reader.optimizer_route_epoch = 1;
  reader.optimizer_route_generation = 1;
  reader.optimizer_memory_budget_bytes = 16 * 1024 * 1024;
  reader.optimizer_maximum_candidate_count = 4096;
  reader.optimizer_maximum_memo_groups = 4096;
  reader.optimizer_maximum_search_steps = 16384;
  reader.optimizer_maximum_planning_time_ns = 1'000'000'000;
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis());
  reader.query_cancellation_requested = [] { return false; };
  AddProductionAuthorization(&reader, fixture.relation_uuid);
  AddProductionAuthorization(&reader, right_relation_uuid);
  const bool timestamp_decoder_exact =
      ProductionTimestampDecoderReconciliation(reader);
  const auto execution = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionColumnarDag(reader, storage)});
  const auto diagnostic = execution.api_result.diagnostics.empty()
                              ? std::string{}
                              : execution.api_result.diagnostics.front().code +
                                    ":" + execution.api_result.diagnostics.front().detail;
  const bool exact =
      execution.profile_matched && execution.optimizer_admitted &&
      execution.optimizer_selected && execution.physical_dag_published &&
      execution.physical_dag_executed && execution.runtime_actuals_attached &&
      execution.canonical_result_published && execution.api_result.ok &&
      execution.physical_node_count == 1 &&
      execution.canonical_result_column_count == 3 &&
      execution.canonical_result_row_count == 3 &&
      execution.api_result.result_shape.rows.size() == 3 &&
      execution.api_result.result_shape.rows[0].fields.size() == 3 &&
      execution.api_result.result_shape.rows[0].fields[0].second.encoded_value ==
          seeds[0].row_uuid &&
      execution.api_result.result_shape.rows[1].fields[2].second.isSqlNull() &&
      execution.api_result.result_shape.rows[2].fields[2].second.encoded_value ==
          "beta";
  const auto join_probe_dag = ProductionColumnarJoinDag(
      reader, storage, right_storage, fixture.salt + 199);
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 20);
  const auto absent_scope = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, join_probe_dag});
  const bool absent_scope_exact =
      absent_scope.profile_matched && !absent_scope.optimizer_selected &&
      !absent_scope.physical_dag_published &&
      !absent_scope.physical_dag_executed &&
      !absent_scope.runtime_actuals_attached &&
      !absent_scope.canonical_result_published &&
      absent_scope.canonical_result_row_count == 0 &&
      !absent_scope.api_result.diagnostics.empty() &&
      absent_scope.api_result.diagnostics.front().code ==
          "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_REQUIRED_V1";
  const auto profiles = ProductionMultilegProfiles(fixture.salt + 70'000);
  const auto mismatched_statement = ProductionUuid(
      platform::UuidKind::object, fixture.salt + 70'500);
  bool mismatched_scope_exact = false;
  {
    opt::MultilegDescriptorDispatchScopeV1 mismatch_scope(
        mismatched_statement, profiles);
    reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 21);
    const auto mismatch = sblr::ExecuteCanonicalCurrentHeapQuery(
        {reader, join_probe_dag});
    mismatched_scope_exact =
        mismatch_scope.installed() && mismatch.profile_matched &&
        !mismatch.optimizer_selected && !mismatch.physical_dag_published &&
        !mismatch.physical_dag_executed &&
        !mismatch.runtime_actuals_attached &&
        !mismatch.canonical_result_published &&
        mismatch.canonical_result_row_count == 0 &&
        !mismatch.api_result.diagnostics.empty() &&
        mismatch.api_result.diagnostics.front().code ==
            "SB_MODEL_RESULT_DESCRIPTOR_SCOPE_MISMATCH_V1";
  }
  opt::MultilegDescriptorDispatchScopeV1 descriptor_scope(
      reader.statement_uuid.canonical, profiles);
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 1);
  const auto joined = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionColumnarJoinDag(
                   reader, storage, right_storage, fixture.salt + 200)});
  const auto join_diagnostic =
      joined.api_result.diagnostics.empty()
          ? std::string{}
          : joined.api_result.diagnostics.front().code + ":" +
                joined.api_result.diagnostics.front().detail;
  const bool joined_exact =
      joined.profile_matched && joined.optimizer_admitted &&
      joined.optimizer_selected && joined.physical_dag_published &&
      joined.physical_dag_executed && joined.runtime_actuals_attached &&
      joined.canonical_result_published && joined.api_result.ok &&
      joined.physical_node_count == 3 &&
      joined.canonical_result_column_count == 6 &&
      joined.canonical_result_row_count == 2 &&
      joined.api_result.result_shape.rows.size() == 2 &&
      joined.api_result.result_shape.rows[0].fields.size() == 6 &&
      joined.api_result.result_shape.rows[0].fields[1].second.encoded_value ==
          "2" &&
      joined.api_result.result_shape.rows[0].fields[4].second.encoded_value ==
          "2" &&
      joined.api_result.result_shape.rows[1].fields[1].second.encoded_value ==
          "3" &&
      joined.api_result.result_shape.rows[1].fields[5].second.encoded_value ==
          "right-three" &&
      HasEvidence(joined, "canonical.model_join_left_provider_route",
                  "canonical.model-provider.columnar.v1") &&
      HasEvidence(joined, "canonical.model_join_right_provider_route",
                  "canonical.model-provider.columnar.v1") &&
      HasEvidence(joined, "canonical.model_join_consumer_route",
                  "canonical.relational.join-3vl-nested.v1") &&
      HasEvidence(joined, "canonical.model_join_condition_route",
                  "canonical.relational.on-typed-predicate.v1");
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 13);
  const auto timezone_equivalent_join =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {reader, ProductionTimezoneEquivalentColumnarJoinDag(
                       reader, storage, right_storage,
                       fixture.salt + 900)});
  const auto timezone_join_diagnostic =
      timezone_equivalent_join.api_result.diagnostics.empty()
          ? std::string{}
          : timezone_equivalent_join.api_result.diagnostics.front().code +
                ":" +
                timezone_equivalent_join.api_result.diagnostics.front().detail;
  const bool timezone_join_exact =
      timezone_equivalent_join.profile_matched &&
      timezone_equivalent_join.optimizer_admitted &&
      timezone_equivalent_join.optimizer_selected &&
      timezone_equivalent_join.physical_dag_published &&
      timezone_equivalent_join.physical_dag_executed &&
      timezone_equivalent_join.runtime_actuals_attached &&
      timezone_equivalent_join.canonical_result_published &&
      timezone_equivalent_join.api_result.ok &&
      timezone_equivalent_join.physical_node_count == 3 &&
      timezone_equivalent_join.canonical_result_column_count == 6 &&
      timezone_equivalent_join.canonical_result_row_count == 9 &&
      HasEvidence(timezone_equivalent_join,
                  "canonical.model_join_consumer_route",
                  "canonical.relational.join-3vl-nested.v1") &&
      HasEvidence(timezone_equivalent_join,
                  "canonical.model_join_condition_route",
                  "canonical.relational.on-typed-predicate.v1");
  const auto execute_common_join =
      [&](const std::uint64_t offset, const std::string_view semantic) {
        reader.current_monotonic_ns =
            std::to_string(ProductionNowMillis() + offset);
        return sblr::ExecuteCanonicalCurrentHeapQuery(
            {reader, ProductionColumnarJoinDag(
                         reader, storage, right_storage,
                         fixture.salt + 600 + offset, true, semantic)});
      };
  const auto left_common =
      execute_common_join(5, "join.left-outer.on.v1");
  const auto right_common =
      execute_common_join(6, "join.right-outer.on.v1");
  const auto full_common =
      execute_common_join(7, "join.full-outer.on.v1");
  const auto semi_common =
      execute_common_join(8, "join.left-semi.on.v1");
  const auto anti_common =
      execute_common_join(9, "join.left-anti.on.v1");
  const auto cross_common = execute_common_join(10, "join.cross.v1");
  const auto using_common =
      execute_common_join(11, "join.inner.using.v1");
  const auto natural_common =
      execute_common_join(12, "join.inner.natural.v1");
  const auto common_join_exact = [&](const auto& candidate,
                                     const std::size_t columns,
                                     const std::size_t rows,
                                     const std::string_view form,
                                     const std::string_view condition,
                                     const std::string_view condition_route) {
    const bool accepted = candidate.profile_matched && candidate.optimizer_admitted &&
           candidate.optimizer_selected &&
           candidate.physical_dag_published &&
           candidate.physical_dag_executed &&
           candidate.runtime_actuals_attached &&
           candidate.canonical_result_published && candidate.api_result.ok &&
           candidate.physical_node_count == 3 &&
           candidate.canonical_result_column_count == columns &&
           candidate.canonical_result_row_count == rows &&
           candidate.api_result.result_shape.rows.size() == rows &&
           HasEvidence(candidate, "canonical.model_join_form", form) &&
           HasEvidence(candidate, "canonical.model_join_condition_form",
                       condition) &&
           HasEvidence(candidate, "canonical.model_join_consumer_route",
                       "canonical.relational.join-3vl-nested.v1") &&
           HasEvidence(candidate, "canonical.model_join_condition_route",
                       condition_route);
    if (!accepted) {
      std::cerr << "QOW-CES05-SPATIAL-COLUMNAR-COMMON: " << form << ':'
                << (candidate.api_result.diagnostics.empty()
                        ? std::string("no-diagnostic")
                        : candidate.api_result.diagnostics.front().code + ":" +
                              candidate.api_result.diagnostics.front().detail)
                << '\n';
    }
    return accepted;
  };
  const auto common_join_refused = [&](const auto& candidate,
                                       const std::string_view code) {
    return candidate.profile_matched && !candidate.optimizer_selected &&
           !candidate.physical_dag_published &&
           !candidate.physical_dag_executed &&
           !candidate.runtime_actuals_attached &&
           !candidate.canonical_result_published &&
           !candidate.api_result.ok &&
           !candidate.api_result.diagnostics.empty() &&
           candidate.api_result.diagnostics.front().code == code;
  };
  const bool all_regular_common_forms =
      common_join_exact(left_common, 6, 3, "LEFT", "ON",
                        "canonical.relational.on-typed-predicate.v1") &&
      common_join_exact(right_common, 6, 3, "RIGHT", "ON",
                        "canonical.relational.on-typed-predicate.v1") &&
      common_join_exact(full_common, 6, 4, "FULL", "ON",
                        "canonical.relational.on-typed-predicate.v1") &&
      common_join_exact(semi_common, 3, 2, "SEMI", "ON",
                        "canonical.relational.on-typed-predicate.v1") &&
      common_join_exact(anti_common, 3, 1, "ANTI", "ON",
                        "canonical.relational.on-typed-predicate.v1") &&
      common_join_exact(cross_common, 6, 9, "CROSS", "NONE",
                        "canonical.relational.cross-no-condition.v1") &&
      common_join_refused(using_common,
                          "SB_MODEL_JOIN_USING_BINDING_REFUSED_V1") &&
      common_join_refused(natural_common,
                          "SB_MODEL_JOIN_NATURAL_BINDING_REFUSED_V1");
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 2);
  const auto relational_joined = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionColumnarJoinDag(
                   reader, storage, right_storage, fixture.salt + 300,
                   false)});
  const auto relational_join_diagnostic =
      relational_joined.api_result.diagnostics.empty()
          ? std::string{}
          : relational_joined.api_result.diagnostics.front().code + ":" +
                relational_joined.api_result.diagnostics.front().detail;
  const bool relational_joined_exact =
      relational_joined.profile_matched &&
      relational_joined.optimizer_admitted &&
      relational_joined.optimizer_selected &&
      relational_joined.physical_dag_published &&
      relational_joined.physical_dag_executed &&
      relational_joined.runtime_actuals_attached &&
      relational_joined.canonical_result_published &&
      relational_joined.api_result.ok &&
      relational_joined.physical_node_count == 3 &&
      relational_joined.canonical_result_column_count == 6 &&
      relational_joined.canonical_result_row_count == 2 &&
      relational_joined.api_result.result_shape.rows.size() == 2 &&
      relational_joined.api_result.result_shape.rows[0].fields.size() == 6 &&
      relational_joined.api_result.result_shape.rows[0]
              .fields[1].second.encoded_value == "2" &&
      relational_joined.api_result.result_shape.rows[0]
              .fields[4].second.encoded_value == "2" &&
      relational_joined.api_result.result_shape.rows[1]
              .fields[1].second.encoded_value == "3" &&
      relational_joined.api_result.result_shape.rows[1]
              .fields[5].second.encoded_value == "right-three" &&
      HasEvidence(relational_joined,
                  "canonical.model_join_left_provider_route",
                  "canonical.model-provider.columnar.v1") &&
      HasEvidence(relational_joined,
                  "canonical.model_join_right_provider_route",
                  "canonical.relational.heap-source.v1") &&
      HasEvidence(relational_joined, "canonical.model_join_consumer_route",
                  "canonical.relational.join-3vl-nested.v1");
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 3);
  const auto lateral_inner = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionColumnarJoinDag(
                   reader, storage, right_storage, fixture.salt + 400, true,
                   "join.lateral-inner-int64-equality.v1")});
  const auto lateral_inner_diagnostic =
      lateral_inner.api_result.diagnostics.empty()
          ? std::string{}
          : lateral_inner.api_result.diagnostics.front().code + ":" +
                lateral_inner.api_result.diagnostics.front().detail;
  const bool lateral_inner_exact =
      lateral_inner.profile_matched && lateral_inner.optimizer_admitted &&
      lateral_inner.optimizer_selected &&
      lateral_inner.physical_dag_published &&
      lateral_inner.physical_dag_executed &&
      lateral_inner.runtime_actuals_attached &&
      lateral_inner.canonical_result_published && lateral_inner.api_result.ok &&
      lateral_inner.physical_node_count == 3 &&
      lateral_inner.canonical_result_row_count == 2 &&
      lateral_inner.api_result.result_shape.rows.size() == 2 &&
      HasEvidence(lateral_inner, "canonical.model_join_form",
                  "LATERAL_INNER") &&
      HasEvidence(lateral_inner, "canonical.model_join_consumer_route",
                  "canonical.relational.lateral-correlated.v1");
  reader.current_monotonic_ns = std::to_string(ProductionNowMillis() + 4);
  const auto lateral_left = sblr::ExecuteCanonicalCurrentHeapQuery(
      {reader, ProductionColumnarJoinDag(
                   reader, storage, right_storage, fixture.salt + 500, true,
                   "join.lateral-left-int64-equality.v1")});
  const auto lateral_left_diagnostic =
      lateral_left.api_result.diagnostics.empty()
          ? std::string{}
          : lateral_left.api_result.diagnostics.front().code + ":" +
                lateral_left.api_result.diagnostics.front().detail;
  const bool lateral_left_exact =
      lateral_left.profile_matched && lateral_left.optimizer_admitted &&
      lateral_left.optimizer_selected && lateral_left.physical_dag_published &&
      lateral_left.physical_dag_executed &&
      lateral_left.runtime_actuals_attached &&
      lateral_left.canonical_result_published && lateral_left.api_result.ok &&
      lateral_left.physical_node_count == 3 &&
      lateral_left.canonical_result_row_count == 3 &&
      lateral_left.api_result.result_shape.rows.size() == 3 &&
      lateral_left.api_result.result_shape.rows[0]
          .fields[3].second.isSqlNull() &&
      lateral_left.api_result.result_shape.rows[0]
          .fields[4].second.isSqlNull() &&
      lateral_left.api_result.result_shape.rows[0]
          .fields[5].second.isSqlNull() &&
      HasEvidence(lateral_left, "canonical.model_join_form",
                  "LATERAL_LEFT") &&
      HasEvidence(lateral_left, "canonical.model_join_consumer_route",
                  "canonical.relational.lateral-correlated.v1");
  const bool rolled_back = ProductionRollback(reader);
  return Require(timestamp_decoder_exact,
                 "timestamp model-graph decoder reconciliation drifted") &&
         Require(exact,
                 "production canonical columnar route drifted: " + diagnostic) &&
         Require(absent_scope_exact,
                 "missing V10 descriptor scope crossed pre-access") &&
         Require(mismatched_scope_exact,
                 "mismatched V10 descriptor scope crossed pre-access") &&
         Require(descriptor_scope.installed(),
                 "production V10 descriptor scope was not installed") &&
         Require(joined_exact,
                 "production canonical columnar join route drifted: " +
                     join_diagnostic) &&
         Require(timezone_join_exact,
                 "production captured model-family timezone-equivalent join "
                 "drifted: " + timezone_join_diagnostic) &&
         Require(all_regular_common_forms,
                 "production canonical regular-form/condition routes drifted") &&
         Require(relational_joined_exact,
                 "production canonical columnar-to-relational join route "
                 "drifted: " + relational_join_diagnostic) &&
         Require(lateral_inner_exact,
                 "production canonical LATERAL INNER route drifted: " +
                     lateral_inner_diagnostic) &&
         Require(lateral_left_exact,
                 "production canonical LATERAL LEFT route drifted: " +
                     lateral_left_diagnostic) &&
         Require(rolled_back,
                 "production columnar reader rollback failed");
}
#endif

}  // namespace

int main() {
#if defined(SB_CES05_SPATIAL_COLUMNAR_PRODUCTION_QUERY_ROUTE)
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "qow_ces05_spatial_columnar";
  const auto memory_configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "qow_ces05_spatial_columnar");
  if (!Require(memory_configured.ok(),
               "default memory fixture configuration failed")) {
    return EXIT_FAILURE;
  }
#endif
  if (!SpatialVectors() || !ColumnarVectors()
#if defined(SB_CES05_SPATIAL_COLUMNAR_PRODUCTION_QUERY_ROUTE)
      || !ProductionDescriptorScopeLifecycle() ||
      !ProductionColumnarRoute() || !ProductionSpatialRoute()
#endif
  ) {
    return EXIT_FAILURE;
  }
  std::cout << "qow_ces05_spatial_columnar=passed\n";
  return EXIT_SUCCESS;
}
