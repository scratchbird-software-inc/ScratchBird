// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"

#include "canonical_relational_expression.hpp"

#include "catalog/name_resolution_api.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
#include "query/canonical_heap_optimizer_admission.hpp"
#include "transaction/transaction_api.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
// Deterministic closure-test seam only.  A one-node selected execution resolves
// current MGA authority at selected-entry, dispatch-entry, node pre/post,
// dispatch-root, actuals entry/result, and immediate pre-result (resolution 8).
// Production builds have no mutable seam and always resolve through the durable
// transaction inventory below.
thread_local bool g_contract_pre_result_revocation_armed = false;
thread_local std::size_t g_contract_revalidation_resolution_count = 0;
thread_local bool g_contract_security_boundary_drift_armed = false;
thread_local bool g_contract_resource_boundary_drift_armed = false;
#endif

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
exec::PhysicalMgaStatementContext PhysicalMgaContextFromResolvedSnapshot(
    const api::EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor) {
  exec::PhysicalMgaStatementContext expected;
  expected.statement_uuid = context.statement_uuid.canonical;
  expected.owning_transaction_uuid = context.transaction_uuid.canonical;
  expected.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  expected.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  expected.owning_local_transaction_id = descriptor.owning_transaction.value;
  expected.visible_committed_high_watermark =
      descriptor.visible_committed_high_watermark;
  expected.oldest_active_transaction_id =
      descriptor.oldest_active_transaction.value;
  expected.oldest_interesting_transaction_id =
      descriptor.oldest_interesting_transaction.value;
  expected.oldest_snapshot_transaction_id =
      descriptor.oldest_snapshot_transaction.value;
  expected.retention_horizon_transaction_id =
      descriptor.retention_horizon_transaction.value;
  expected.active_excluded_local_transaction_ids =
      descriptor.active_excluded_local_transaction_ids;
  expected.in_doubt_excluded_local_transaction_ids =
      descriptor.in_doubt_excluded_local_transaction_ids;
  expected.snapshot_kind =
      scratchbird::transaction::mga::SnapshotVectorKindName(
          descriptor.snapshot_kind);
  expected.publication_inventory_next_local_transaction_id =
      descriptor.publication_inventory_next_local_transaction_id;
  expected.inventory_authoritative = descriptor.inventory_authoritative;
  expected.complete = descriptor.complete;
  expected.current = true;
  return expected;
}
#endif

exec::CanonicalExecutionMgaAuthority BuildCanonicalExecutionMgaAuthority(
    const api::EngineRequestContext& context,
    const exec::TypedPhysicalNodeDag& physical_dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = physical_dag.mga_statement_context;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [expected = authority.statement_context]() {
    exec::CanonicalMgaCurrentResolution resolved;
    resolved.statement_context = expected;
    ++g_contract_revalidation_resolution_count;
    if (g_contract_pre_result_revocation_armed &&
        g_contract_revalidation_resolution_count == 8) {
      resolved.statement_context.current = false;
      g_contract_pre_result_revocation_armed = false;
    }
    return resolved;
  };
#else
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  authority.resolve_current = [context]() {
    exec::CanonicalMgaCurrentResolution current;
    api::EngineResolveStatementSnapshotRequest resolve_request;
    resolve_request.context = context;
    const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
    if (!resolved.ok) {
      current.diagnostic.ok = false;
      current.diagnostic.diagnostic_code =
          "SB_DIAG_MGA_READ_SNAPSHOT_MISSING";
      current.diagnostic.detail =
          "statement snapshot is unknown, revoked, stale, or not current";
      return current;
    }
    current.statement_context =
        PhysicalMgaContextFromResolvedSnapshot(context,
                                               resolved.snapshot_vector);
    return current;
  };
#endif
  return authority;
}

api::CanonicalOptimizerSelectedExecutionResult ExecuteSelectedWithMgaGuard(
    const api::EngineRequestContext& context,
    const api::CanonicalOptimizerSelectedExecutionRequest& request) {
  auto bounded_request = request;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  // Deterministic closure seams model a selected plan becoming stale between
  // publication and execution. Production has no mutable selected-plan seam.
  if (g_contract_security_boundary_drift_armed) {
    ++bounded_request.selected_physical_dag.security_epoch;
    g_contract_security_boundary_drift_armed = false;
  }
  if (g_contract_resource_boundary_drift_armed) {
    ++bounded_request.selected_physical_dag.resource_epoch;
    g_contract_resource_boundary_drift_armed = false;
  }
#endif

  const auto refuse_boundary = [](std::string field_id) {
    api::CanonicalOptimizerSelectedExecutionResult result;
    result.issues.push_back(
        {"QOW-DIAG-QRY-004-SELECT-EXECUTION-BOUNDARY-V1", 0,
         std::move(field_id)});
    return result;
  };
  const auto& dag = bounded_request.selected_physical_dag;
  const auto& authorization = context.authorization_context;
  if (!context.security_context_present || !authorization.present ||
      authorization.authority_uuid.canonical.empty() ||
      dag.security_context_uuid != authorization.authority_uuid.canonical ||
      context.principal_uuid.canonical !=
          authorization.principal_uuid.canonical ||
      context.security_epoch == 0 ||
      dag.security_epoch != context.security_epoch ||
      authorization.security_epoch != context.security_epoch ||
      authorization.policy_epoch == 0 ||
      dag.policy_epoch != authorization.policy_epoch ||
      context.catalog_generation_id == 0 ||
      dag.catalog_generation != context.catalog_generation_id ||
      authorization.catalog_generation_id != context.catalog_generation_id) {
    return refuse_boundary("security_authorization_binding");
  }
  if (context.resource_epoch == 0 ||
      dag.resource_epoch != context.resource_epoch ||
      context.optimizer_resource_snapshot_uuid.canonical.empty() ||
      dag.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      context.optimizer_memory_budget_bytes == 0 ||
      dag.memory_budget_bytes != context.optimizer_memory_budget_bytes) {
    return refuse_boundary("resource_budget_binding");
  }

  std::size_t maximum_output_width = 0;
  for (const auto& node : dag.nodes) {
    maximum_output_width =
        std::max(maximum_output_width, node.output_descriptor_ids.size());
  }
  const auto bounded_budget = static_cast<std::size_t>(
      std::min<std::uint64_t>(context.optimizer_memory_budget_bytes,
                              std::numeric_limits<std::size_t>::max()));
  if (maximum_output_width == 0 || bounded_budget < maximum_output_width) {
    return refuse_boundary("runtime_materialization_budget");
  }
  auto& runtime_limits = bounded_request.runtime_limits;
  runtime_limits.maximum_columns_per_batch =
      std::min(runtime_limits.maximum_columns_per_batch, maximum_output_width);
  runtime_limits.maximum_cells_per_batch =
      std::min(runtime_limits.maximum_cells_per_batch, bounded_budget);
  runtime_limits.maximum_rows_per_batch = std::min(
      runtime_limits.maximum_rows_per_batch,
      runtime_limits.maximum_cells_per_batch / maximum_output_width);
  runtime_limits.maximum_total_materialized_cells = std::min(
      runtime_limits.maximum_total_materialized_cells, bounded_budget);
  runtime_limits.maximum_total_materialized_rows = std::min(
      runtime_limits.maximum_total_materialized_rows, bounded_budget);
  if (runtime_limits.maximum_columns_per_batch == 0 ||
      runtime_limits.maximum_cells_per_batch == 0 ||
      runtime_limits.maximum_rows_per_batch == 0 ||
      runtime_limits.maximum_total_materialized_cells == 0 ||
      runtime_limits.maximum_total_materialized_rows == 0) {
    return refuse_boundary("runtime_materialization_budget");
  }
  bounded_request.cancellation_requested =
      context.query_cancellation_requested
          ? context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  return api::ExecuteCanonicalOptimizerSelectedDag(bounded_request);
#else
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  return api::ExecuteCanonicalOptimizerSelectedDag(bounded_request);
#endif
}

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

api::EngineApiResult Failure(const CanonicalObjectFreeValuesExecutionRequest& request,
                             std::string diagnostic_id,
                             std::string detail) {
  api::EngineApiResult result;
  result.operation_id = "query.execute";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(diagnostic_id);
  diagnostic.message_key = "engine.sblr.query_execute.refused";
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

exec::CanonicalResultNullability ResultNullability(
    const api::RelationalNullability nullability) {
  switch (nullability) {
    case api::RelationalNullability::kNonNull:
      return exec::CanonicalResultNullability::kNonNull;
    case api::RelationalNullability::kNullable:
      return exec::CanonicalResultNullability::kNullable;
    case api::RelationalNullability::kUnknown:
      return exec::CanonicalResultNullability::kUnknown;
  }
  return exec::CanonicalResultNullability::kUnknown;
}

struct MaterializedValues {
  bool ok{false};
  exec::DescriptorBatch batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedSetOperationRoot {
  bool ok{false};
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::vector<exec::CanonicalSetOperationCollationBinding>
      collation_bindings;
  std::string detail;
};

struct LiveSetOperationProfile {
  bool matched{false};
  exec::CanonicalSetOperationKind operation =
      exec::CanonicalSetOperationKind::kUnion;
  exec::CanonicalSetOperationAlignment alignment =
      exec::CanonicalSetOperationAlignment::kOrdinal;
  exec::CanonicalSetOperationQuantifier quantifier =
      exec::CanonicalSetOperationQuantifier::kAll;
  exec::CanonicalSetOperationEqualityProfile equality_profile =
      exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  exec::CanonicalSetOperationTypeProfile type_profile =
      exec::CanonicalSetOperationTypeProfile::kExact;
  std::string implementation_id;
  std::string physical_semantic_id;
  std::string identity_component;
  std::string operation_name;
};

struct PreparedLiveSetNode {
  LiveSetOperationProfile profile;
  PreparedSetOperationRoot prepared;
  std::size_t maximum_output_row_count{0};
  std::size_t maximum_equality_comparison_count{0};
};

struct MaterializedSetOperationPlanningState {
  MaterializedValues values;
  std::uint64_t output_bound{0};
  std::uint64_t comparison_bound{0};
  std::uint64_t work_bound{0};
};

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                std::uint64_t* result);
bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result);

MaterializedSetOperationPlanningState MaterializeSetOperationPlanningState(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  MaterializedSetOperationPlanningState result;
  if (!prepared.ok || !left.ok || !right.ok ||
      (profile.alignment != exec::CanonicalSetOperationAlignment::kOrdinal &&
       profile.alignment !=
           exec::CanonicalSetOperationAlignment::kByName) ||
      (profile.type_profile !=
           exec::CanonicalSetOperationTypeProfile::kExact &&
       profile.type_profile !=
           exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) ||
      (profile.equality_profile !=
           exec::CanonicalSetOperationEqualityProfile::kExactTyped &&
       profile.equality_profile !=
           exec::CanonicalSetOperationEqualityProfile::
               kNullEqualBoundCollation) ||
      !CheckedAdd(left.batch.rows.size(), right.batch.rows.size(),
                  &result.output_bound) ||
      !CheckedMultiply(result.output_bound, result.output_bound,
                       &result.comparison_bound)) {
    result.values.detail =
        "set-operation planning-state contract or bound is invalid";
    return result;
  }
  result.work_bound =
      profile.operation == exec::CanonicalSetOperationKind::kUnion &&
              profile.quantifier ==
                  exec::CanonicalSetOperationQuantifier::kAll
          ? result.output_bound
          : result.comparison_bound;

  result.values.ok = true;
  result.values.batch.columns = prepared.result_columns;
  result.values.result_bindings = prepared.result_bindings;
  const auto retagged_rows = [&](const exec::DescriptorBatch& batch) {
    std::vector<exec::DescriptorTuple> rows;
    rows.reserve(batch.rows.size());
    for (const auto& source : batch.rows) {
      auto row = source;
      for (std::size_t column = 0; column < row.values.size(); ++column) {
        row.values[column].descriptor =
            prepared.result_columns[column].descriptor;
      }
      rows.push_back(std::move(row));
    }
    return rows;
  };
  exec::DescriptorBatch reconciled_left = left.batch;
  exec::DescriptorBatch aligned_right = right.batch;
  if (profile.alignment ==
      exec::CanonicalSetOperationAlignment::kByName) {
    std::unordered_map<std::string, std::size_t> right_ordinals;
    for (std::size_t column = 0; column < right.batch.columns.size();
         ++column) {
      if (right.batch.columns[column].stable_name.empty() ||
          !right_ordinals
               .emplace(right.batch.columns[column].stable_name, column)
               .second) {
        result.values = {};
        result.values.detail =
            "BY NAME set-operation right names are not unique";
        return result;
      }
    }
    aligned_right.columns.clear();
    aligned_right.rows.assign(right.batch.rows.size(), {});
    for (const auto& result_column : prepared.result_columns) {
      const auto found = right_ordinals.find(result_column.stable_name);
      if (found == right_ordinals.end()) {
        result.values = {};
        result.values.detail =
            "BY NAME set-operation column sets differ";
        return result;
      }
      aligned_right.columns.push_back(right.batch.columns[found->second]);
      for (std::size_t row = 0; row < right.batch.rows.size(); ++row) {
        aligned_right.rows[row].values.push_back(
            right.batch.rows[row].values[found->second]);
      }
    }
  }
  if (profile.type_profile ==
      exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) {
    std::string conversion_detail;
    const auto reconcile_batch = [&](exec::DescriptorBatch* batch) {
      if (batch == nullptr ||
          batch->columns.size() != prepared.result_columns.size()) {
        conversion_detail =
            "type-reconciled set-operation arity changed";
        return false;
      }
      for (std::size_t column = 0; column < batch->columns.size(); ++column) {
        const auto source_type = dt::CanonicalTypeIdFromStableName(
            batch->columns[column].descriptor.canonical_type_name);
        const auto target_type = dt::CanonicalTypeIdFromStableName(
            prepared.result_columns[column].descriptor.canonical_type_name);
        const auto category =
            dt::ClassifyDatatypeCast(source_type, target_type);
        if (source_type == dt::CanonicalTypeId::unknown ||
            target_type == dt::CanonicalTypeId::unknown ||
            (category != dt::DatatypeCastCategory::identity &&
             category !=
                 dt::DatatypeCastCategory::lossless_implicit)) {
          conversion_detail =
              "set-operation planning cast is not lossless implicit";
          return false;
        }
        for (auto& row : batch->rows) {
          dt::DatatypeCastRequest conversion;
          conversion.value.type_id = source_type;
          conversion.value.encoded_value =
              row.values[column].encoded_value;
          conversion.value.is_null =
              row.values[column].state ==
              api::EngineValueState::sql_null;
          conversion.target_type_id = target_type;
          const auto cast = dt::CastDatatypeValue(conversion);
          if (!cast.ok()) {
            conversion_detail =
                cast.diagnostic.diagnostic_code.empty()
                    ? "set-operation planning lossless cast refused"
                    : cast.diagnostic.diagnostic_code;
            return false;
          }
          row.values[column].descriptor =
              prepared.result_columns[column].descriptor;
          row.values[column].encoded_value = cast.value.encoded_value;
          row.values[column].binary_value.clear();
          row.values[column].is_null = cast.value.is_null;
          row.values[column].state =
              cast.value.is_null ? api::EngineValueState::sql_null
                                 : api::EngineValueState::value;
        }
        batch->columns[column].descriptor =
            prepared.result_columns[column].descriptor;
        batch->columns[column].nullable =
            prepared.result_columns[column].nullable;
      }
      return true;
    };
    if (!reconcile_batch(&reconciled_left) ||
        !reconcile_batch(&aligned_right)) {
      result.values = {};
      result.values.detail = std::move(conversion_detail);
      return result;
    }
    std::uint64_t conversion_count = 0;
    if (!CheckedMultiply(result.output_bound,
                         prepared.result_columns.size(),
                         &conversion_count) ||
        !CheckedAdd(result.work_bound, conversion_count,
                    &result.work_bound)) {
      result.values = {};
      result.values.detail =
          "set-operation conversion work bound overflowed";
      return result;
    }
  }
  const auto left_rows = retagged_rows(reconciled_left);
  const auto right_rows = retagged_rows(aligned_right);
  using SetRowKey = std::vector<std::string>;
  const auto row_key = [](const exec::DescriptorTuple& row) {
    SetRowKey key;
    key.reserve(row.values.size());
    for (const auto& value : row.values) {
      if (value.state == api::EngineValueState::sql_null) {
        key.emplace_back("null");
        continue;
      }
      std::string token = value.descriptor.canonical_type_name + ":" +
                          value.encoded_value + ":";
      token.append(reinterpret_cast<const char*>(value.binary_value.data()),
                   value.binary_value.size());
      key.push_back(std::move(token));
    }
    return key;
  };
  std::vector<SetRowKey> left_keys;
  std::vector<SetRowKey> right_keys;
  left_keys.reserve(left_rows.size());
  right_keys.reserve(right_rows.size());
  for (const auto& row : left_rows) left_keys.push_back(row_key(row));
  for (const auto& row : right_rows) right_keys.push_back(row_key(row));

  if (profile.equality_profile ==
      exec::CanonicalSetOperationEqualityProfile::
          kNullEqualBoundCollation) {
    for (const auto& binding : prepared.collation_bindings) {
      if (binding.result_column >= prepared.result_columns.size()) {
        result.values = {};
        result.values.detail =
            "set-operation planning collation column is out of range";
        return result;
      }
      std::vector<const api::EngineTypedValue*> representatives;
      const auto classify = [&](const api::EngineTypedValue& value,
                                std::string* equality_key) {
        if (equality_key == nullptr) return false;
        if (value.state == api::EngineValueState::sql_null) {
          *equality_key = "collation:null";
          return true;
        }
        for (std::size_t index = 0; index < representatives.size(); ++index) {
          dt::DatatypeComparisonRequest comparison_request;
          comparison_request.left.type_id = dt::CanonicalTypeId::character;
          comparison_request.left.encoded_value =
              representatives[index]->encoded_value;
          comparison_request.right.type_id = dt::CanonicalTypeId::character;
          comparison_request.right.encoded_value = value.encoded_value;
          comparison_request.case_insensitive_character_compare =
              binding.text_seed.collation_case_insensitive;
          comparison_request.text_seed = binding.text_seed;
          const auto compared = dt::CompareDatatypeValues(comparison_request);
          if (!compared.ok()) {
            result.values.detail =
                compared.diagnostic.diagnostic_code.empty()
                    ? "set-operation planning collation comparison refused"
                    : compared.diagnostic.diagnostic_code;
            return false;
          }
          if (compared.comparison == 0) {
            *equality_key = "collation:" + std::to_string(index);
            return true;
          }
        }
        representatives.push_back(&value);
        *equality_key =
            "collation:" + std::to_string(representatives.size() - 1);
        return true;
      };
      const auto classify_rows = [&](const std::vector<exec::DescriptorTuple>& rows,
                                     std::vector<SetRowKey>* keys) {
        if (keys == nullptr || keys->size() != rows.size()) return false;
        for (std::size_t row = 0; row < rows.size(); ++row) {
          if (!classify(rows[row].values[binding.result_column],
                        &(*keys)[row][binding.result_column])) {
            return false;
          }
        }
        return true;
      };
      if (!classify_rows(left_rows, &left_keys) ||
          !classify_rows(right_rows, &right_keys)) {
        const auto detail = result.values.detail.empty()
                                ? "set-operation planning collation key "
                                  "classification refused"
                                : result.values.detail;
        result.values = {};
        result.values.detail = detail;
        return result;
      }
    }
  }

  result.values.batch.rows.reserve(result.output_bound);
  const bool distinct =
      profile.quantifier ==
      exec::CanonicalSetOperationQuantifier::kDistinct;
  if (distinct &&
      profile.operation == exec::CanonicalSetOperationKind::kUnion) {
    std::set<SetRowKey> emitted;
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      if (emitted.insert(left_keys[row]).second) {
        result.values.batch.rows.push_back(left_rows[row]);
      }
    }
    for (std::size_t row = 0; row < right_rows.size(); ++row) {
      if (emitted.insert(right_keys[row]).second) {
        result.values.batch.rows.push_back(right_rows[row]);
      }
    }
  } else if (distinct) {
    std::set<SetRowKey> right_membership(right_keys.begin(), right_keys.end());
    std::set<SetRowKey> emitted;
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      const bool present = right_membership.contains(left_keys[row]);
      const bool candidate =
          profile.operation == exec::CanonicalSetOperationKind::kIntersect
              ? present
              : !present;
      if (candidate && emitted.insert(left_keys[row]).second) {
        result.values.batch.rows.push_back(left_rows[row]);
      }
    }
  } else if (profile.operation ==
             exec::CanonicalSetOperationKind::kUnion) {
    result.values.batch.rows.insert(result.values.batch.rows.end(),
                                    left_rows.begin(), left_rows.end());
    result.values.batch.rows.insert(result.values.batch.rows.end(),
                                    right_rows.begin(), right_rows.end());
  } else {
    std::map<SetRowKey, std::size_t> right_multiplicity;
    for (const auto& key : right_keys) ++right_multiplicity[key];
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      auto found = right_multiplicity.find(left_keys[row]);
      const bool consumes =
          found != right_multiplicity.end() && found->second != 0;
      if (consumes) --found->second;
      const bool emit =
          profile.operation == exec::CanonicalSetOperationKind::kIntersect
              ? consumes
              : !consumes;
      if (emit) result.values.batch.rows.push_back(left_rows[row]);
    }
  }
  return result;
}

struct PreparedJoinRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_row_binding;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedFilterRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_row_binding;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedProjectExpression {
  std::uint32_t expression_id{0};
  std::string expected_type;
  CanonicalRelationalExpressionRowBinding row_binding;
};

struct PreparedProjectRoot {
  bool ok{false};
  bool expression_projection{false};
  std::vector<std::size_t> projected_columns;
  std::vector<PreparedProjectExpression> expressions;
  exec::DescriptorBatch expression_output_batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedLimitRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedSortExpression {
  std::uint32_t expression_id{0};
  std::string expected_type;
  CanonicalRelationalExpressionRowBinding row_binding;
  exec::ExecutorColumnDescriptor materialized_column;
};

struct PreparedSortRoot {
  bool ok{false};
  bool expression_ordering{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> order_terms;
  std::vector<PreparedSortExpression> expressions;
  exec::DescriptorBatch expression_input_batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string ordering_property_uuid;
  std::string detail;
};

struct PreparedDistinctRoot {
  bool ok{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedGlobalAggregateRoot {
  bool ok{false};
  bool count_star{false};
  bool distinct{false};
  std::vector<std::size_t> value_columns;
  std::vector<std::uint32_t> value_descriptor_ids;
  std::vector<api::EngineTypedValue> direct_arguments;
  std::optional<std::size_t> filter_column;
  std::uint32_t filter_descriptor_id{0};
  std::vector<exec::CanonicalDescriptorOrderTerm> aggregate_order_terms;
  std::string aggregate_separator{","};
  exec::CanonicalListaggOverflowMode listagg_overflow_mode =
      exec::CanonicalListaggOverflowMode::none;
  std::size_t listagg_max_output_bytes{0};
  std::string listagg_truncation_indicator{"..."};
  bool listagg_with_count{true};
  exec::CanonicalAggregateDescriptor aggregate_descriptor;
  exec::ExecutorColumnDescriptor result_column;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedGroupedCountSumRoot {
  bool ok{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> key_terms;
  std::vector<exec::ExecutorColumnDescriptor> key_result_columns;
  std::vector<exec::CanonicalAggregateGroupingSet> grouping_sets;
  std::vector<exec::ExecutorColumnDescriptor> grouping_projection_columns;
  PreparedGlobalAggregateRoot count;
  PreparedGlobalAggregateRoot sum;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedGroupedHavingRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding row_binding;
  std::size_t output_column_count{0};
  std::string detail;
};

struct LiveGroupedCountSumProfile {
  bool matched{false};
  std::size_t key_count{0};
  exec::CanonicalAggregateGroupingExpansionKind expansion_kind =
      exec::CanonicalAggregateGroupingExpansionKind::explicit_sets;
  std::vector<exec::CanonicalAggregateGroupingSet> grouping_sets;
  bool grouping_sets_from_sblr{false};
  bool projects_grouping_metadata{false};
  std::string transformation_id;
};

LiveGroupedCountSumProfile MatchLiveGroupedCountSumProfile(
    const std::string_view semantic_variant_id) {
  LiveGroupedCountSumProfile result;
  if (semantic_variant_id ==
      "aggregate.grouped-int64-key-count-sum.v1") {
    result.matched = true;
    result.key_count = 1;
    result.grouping_sets = {{{0}}};
    result.transformation_id =
        "canonical.aggregate.grouped-int64-key-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouped-int64-keys-count-sum.v1") {
    // QOW-SOURCE-QRY-005-LIVE-TWO-KEY-GROUP-BY-V1
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets = {{{0, 1}}};
    result.transformation_id =
        "canonical.aggregate.grouped-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::rollup;
    result.transformation_id =
        "canonical.aggregate.rollup-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::rollup;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.rollup-int64-keys-count-sum-grouping.v1";
  } else if (semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::cube;
    result.transformation_id =
        "canonical.aggregate.cube-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.expansion_kind =
        exec::CanonicalAggregateGroupingExpansionKind::cube;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.cube-int64-keys-count-sum-grouping.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum.v1") {
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets_from_sblr = true;
    result.transformation_id =
        "canonical.aggregate.grouping-sets-int64-keys-count-sum.v1";
  } else if (semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") {
    result.matched = true;
    result.key_count = 2;
    result.grouping_sets_from_sblr = true;
    result.projects_grouping_metadata = true;
    result.transformation_id =
        "canonical.aggregate.grouping-sets-int64-keys-count-sum-grouping.v1";
  }
  return result;
}

bool IsLiveGroupedHavingProfile(const std::string_view semantic_variant_id) {
  return semantic_variant_id ==
             "filter.having-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-count-sum-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-not-sum-count-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-sum-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-gt-int64-literal.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-not-count-sum-or-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-count-sum-and-gt-int64-literals.v1" ||
         semantic_variant_id ==
             "filter.having-count-sum-or-gt-int64-literals.v1";
}

struct LiveUnaryAggregateExpressionProfile {
  bool matched{false};
  bool count_star{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

LiveUnaryAggregateExpressionProfile MatchLiveUnaryAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveUnaryAggregateExpressionProfile result;
  if (semantic_variant_id == "aggregate.global-count-star.v1") {
    result.matched = true;
    result.count_star = true;
    result.function = exec::CanonicalAggregateFunction::count;
    result.transformation_id =
        "canonical.aggregate.global-count-star.v1";
    return result;
  }

  struct FunctionProfile {
    std::string_view stem;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 14> kFunctionProfiles = {{
      {"count", exec::CanonicalAggregateFunction::count},
      {"sum", exec::CanonicalAggregateFunction::sum},
      {"avg", exec::CanonicalAggregateFunction::avg},
      {"min", exec::CanonicalAggregateFunction::min},
      {"max", exec::CanonicalAggregateFunction::max},
      {"bool-and", exec::CanonicalAggregateFunction::bool_and},
      {"bool-or", exec::CanonicalAggregateFunction::bool_or},
      {"every", exec::CanonicalAggregateFunction::every},
      {"stddev-pop", exec::CanonicalAggregateFunction::stddev_pop},
      {"variance-pop", exec::CanonicalAggregateFunction::variance_pop},
      {"stddev", exec::CanonicalAggregateFunction::stddev},
      {"variance", exec::CanonicalAggregateFunction::variance},
      {"stddev-samp", exec::CanonicalAggregateFunction::stddev_samp},
      {"variance-samp", exec::CanonicalAggregateFunction::variance_samp},
  }};

  for (const auto& profile : kFunctionProfiles) {
    const std::string prefix =
        "aggregate.global-" + std::string(profile.stem);
    if (semantic_variant_id == prefix + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id == prefix + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LivePairStatisticalExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

LivePairStatisticalExpressionProfile MatchLivePairStatisticalExpressionProfile(
    const std::string_view semantic_variant_id) {
  LivePairStatisticalExpressionProfile result;
  struct FunctionProfile {
    std::string_view stem;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 12> kFunctionProfiles = {{
      {"corr", exec::CanonicalAggregateFunction::corr},
      {"covar-pop", exec::CanonicalAggregateFunction::covar_pop},
      {"covar-samp", exec::CanonicalAggregateFunction::covar_samp},
      {"regr-count", exec::CanonicalAggregateFunction::regr_count},
      {"regr-avgx", exec::CanonicalAggregateFunction::regr_avgx},
      {"regr-avgy", exec::CanonicalAggregateFunction::regr_avgy},
      {"regr-intercept", exec::CanonicalAggregateFunction::regr_intercept},
      {"regr-r2", exec::CanonicalAggregateFunction::regr_r2},
      {"regr-slope", exec::CanonicalAggregateFunction::regr_slope},
      {"regr-sxx", exec::CanonicalAggregateFunction::regr_sxx},
      {"regr-sxy", exec::CanonicalAggregateFunction::regr_sxy},
      {"regr-syy", exec::CanonicalAggregateFunction::regr_syy},
  }};

  for (const auto& profile : kFunctionProfiles) {
    const std::string prefix =
        "aggregate.global-" + std::string(profile.stem);
    if (semantic_variant_id == prefix + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id == prefix + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               prefix + "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LiveStringAggregateExpressionProfile {
  bool matched{false};
  bool ordered{false};
  bool distinct{false};
  bool has_filter{false};
  std::string transformation_id;
};

LiveStringAggregateExpressionProfile MatchLiveStringAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveStringAggregateExpressionProfile result;
  struct OrderProfile {
    std::string_view prefix;
    bool ordered;
  };
  static constexpr std::array<OrderProfile, 2> kOrderProfiles = {{
      {"aggregate.global-string-agg", false},
      {"aggregate.global-string-agg-ordered", true},
  }};

  for (const auto& profile : kOrderProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.ordered = profile.ordered;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LiveOrderedSingleCollectionExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

LiveOrderedSingleCollectionExpressionProfile
MatchLiveOrderedSingleCollectionExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveOrderedSingleCollectionExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 2> kFunctionProfiles = {{
      {"aggregate.global-array-agg-ordered",
       exec::CanonicalAggregateFunction::array_agg},
      {"aggregate.global-json-agg-ordered",
       exec::CanonicalAggregateFunction::json_agg},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LiveJsonObjectAggregateExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  std::string transformation_id;
};

LiveJsonObjectAggregateExpressionProfile
MatchLiveJsonObjectAggregateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveJsonObjectAggregateExpressionProfile result;
  constexpr std::string_view kPrefix =
      "aggregate.global-json-object-agg-ordered";
  if (semantic_variant_id == std::string(kPrefix) + "-expression.v1") {
    result.matched = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) + "-filter-expression.v1") {
    result.matched = true;
    result.has_filter = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) + "-distinct-expression.v1") {
    result.matched = true;
    result.distinct = true;
  } else if (semantic_variant_id ==
             std::string(kPrefix) +
                 "-distinct-filter-expression.v1") {
    result.matched = true;
    result.distinct = true;
    result.has_filter = true;
  }
  if (result.matched) {
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
  }
  return result;
}

struct LiveListaggExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  std::size_t base_argument_count{0};
  exec::CanonicalListaggOverflowMode overflow_mode =
      exec::CanonicalListaggOverflowMode::none;
  std::string transformation_id;
};

LiveListaggExpressionProfile MatchLiveListaggExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveListaggExpressionProfile result;
  struct FormProfile {
    std::string_view prefix;
    std::size_t base_argument_count;
    exec::CanonicalListaggOverflowMode overflow_mode;
  };
  static constexpr std::array<FormProfile, 3> kFormProfiles = {{
      {"aggregate.global-listagg-ordered", 3,
       exec::CanonicalListaggOverflowMode::none},
      {"aggregate.global-listagg-ordered-overflow-error", 4,
       exec::CanonicalListaggOverflowMode::error},
      {"aggregate.global-listagg-ordered-overflow-truncate", 6,
       exec::CanonicalListaggOverflowMode::truncate},
  }};

  for (const auto& profile : kFormProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.base_argument_count = profile.base_argument_count;
    result.overflow_mode = profile.overflow_mode;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LiveOrderedSetExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

LiveOrderedSetExpressionProfile MatchLiveOrderedSetExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveOrderedSetExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 7> kFunctionProfiles = {{
      {"aggregate.global-rank-hypothetical",
       exec::CanonicalAggregateFunction::rank},
      {"aggregate.global-dense-rank-hypothetical",
       exec::CanonicalAggregateFunction::dense_rank},
      {"aggregate.global-percent-rank-hypothetical",
       exec::CanonicalAggregateFunction::percent_rank},
      {"aggregate.global-cume-dist-hypothetical",
       exec::CanonicalAggregateFunction::cume_dist},
      {"aggregate.global-mode-ordered",
       exec::CanonicalAggregateFunction::mode},
      {"aggregate.global-percentile-cont-ordered",
       exec::CanonicalAggregateFunction::percentile_cont},
      {"aggregate.global-percentile-disc-ordered",
       exec::CanonicalAggregateFunction::percentile_disc},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

struct LiveApproximateExpressionProfile {
  bool matched{false};
  bool distinct{false};
  bool has_filter{false};
  exec::CanonicalAggregateFunction function =
      exec::CanonicalAggregateFunction::unknown;
  std::string transformation_id;
};

LiveApproximateExpressionProfile MatchLiveApproximateExpressionProfile(
    const std::string_view semantic_variant_id) {
  LiveApproximateExpressionProfile result;
  struct FunctionProfile {
    std::string_view prefix;
    exec::CanonicalAggregateFunction function;
  };
  static constexpr std::array<FunctionProfile, 5> kFunctionProfiles = {{
      {"aggregate.global-approx-count-distinct",
       exec::CanonicalAggregateFunction::approx_count_distinct},
      {"aggregate.global-approx-median",
       exec::CanonicalAggregateFunction::approx_median},
      {"aggregate.global-approx-percentile-cont-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_cont},
      {"aggregate.global-approx-percentile-disc-ordered",
       exec::CanonicalAggregateFunction::approx_percentile_disc},
      {"aggregate.global-approx-top-k",
       exec::CanonicalAggregateFunction::approx_top_k},
  }};

  for (const auto& profile : kFunctionProfiles) {
    if (semantic_variant_id ==
        std::string(profile.prefix) + "-expression.v1") {
      result.matched = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-filter-expression.v1") {
      result.matched = true;
      result.has_filter = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) + "-distinct-expression.v1") {
      result.matched = true;
      result.distinct = true;
    } else if (semantic_variant_id ==
               std::string(profile.prefix) +
                   "-distinct-filter-expression.v1") {
      result.matched = true;
      result.distinct = true;
      result.has_filter = true;
    }
    if (!result.matched) continue;
    result.function = profile.function;
    result.transformation_id =
        "canonical." + std::string(semantic_variant_id);
    return result;
  }
  return result;
}

bool MaterializeAggregateFilterTruthValues(
    const exec::DescriptorBatch& input,
    const std::size_t filter_column,
    const std::uint32_t filter_descriptor_id,
    std::vector<api::EngineSqlTruthValue>* filter_truth_values,
    std::string* detail) {
  if (filter_truth_values == nullptr || detail == nullptr) return false;
  filter_truth_values->clear();
  if (filter_column >= input.columns.size() ||
      input.columns[filter_column].descriptor_id != filter_descriptor_id ||
      input.columns[filter_column].descriptor.canonical_type_name !=
          "boolean") {
    *detail =
        "global aggregate FILTER input descriptor is not exact canonical "
        "boolean";
    return false;
  }
  filter_truth_values->reserve(input.rows.size());
  for (const auto& row : input.rows) {
    if (filter_column >= row.values.size()) {
      *detail = "global aggregate FILTER input cardinality is unresolved";
      return false;
    }
    const auto& value = row.values[filter_column];
    api::EngineCanonicalExpressionEvaluationRequest expression_request;
    expression_request.consumer =
        api::EngineCanonicalExpressionConsumer::aggregate;
    expression_request.operation =
        api::EngineCanonicalExpressionOperation::identity;
    expression_request.left_value = value;
    expression_request.result_descriptor = value.descriptor;
    api::EngineCanonicalExpressionEvaluationResult expression_result;
    api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unspecified;
    if (!api::QowEvaluateCanonicalTypedExpressionV1(
            expression_request, &expression_result, detail) ||
        !api::QowCanonicalTruthFromTypedValueV1(
            expression_result.value, &truth, detail)) {
      const std::string expression_detail = *detail;
      *detail =
          "global aggregate FILTER input is not exact SQL boolean "
          "three-valued state: " + expression_detail;
      return false;
    }
    filter_truth_values->push_back(truth);
  }
  return true;
}

PreparedGlobalAggregateRoot PrepareGlobalAggregateRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const exec::CanonicalAggregateFunction function,
    const bool count_star,
    const bool distinct,
    const bool has_filter,
    const std::uint32_t expected_output_ordinal = 0,
    const bool allow_sibling_outputs = false) {
  PreparedGlobalAggregateRoot result;
  result.count_star = count_star;
  result.distinct = distinct;
  const bool is_count = function == exec::CanonicalAggregateFunction::count;
  const bool is_sum = function == exec::CanonicalAggregateFunction::sum;
  const bool is_avg = function == exec::CanonicalAggregateFunction::avg;
  const bool is_min = function == exec::CanonicalAggregateFunction::min;
  const bool is_max = function == exec::CanonicalAggregateFunction::max;
  const bool is_bool_and =
      function == exec::CanonicalAggregateFunction::bool_and;
  const bool is_bool_or =
      function == exec::CanonicalAggregateFunction::bool_or;
  const bool is_every = function == exec::CanonicalAggregateFunction::every;
  const bool is_boolean = is_bool_and || is_bool_or || is_every;
  const bool is_stddev_pop =
      function == exec::CanonicalAggregateFunction::stddev_pop;
  const bool is_variance_pop =
      function == exec::CanonicalAggregateFunction::variance_pop;
  const bool is_stddev =
      function == exec::CanonicalAggregateFunction::stddev;
  const bool is_variance =
      function == exec::CanonicalAggregateFunction::variance;
  const bool is_stddev_samp =
      function == exec::CanonicalAggregateFunction::stddev_samp;
  const bool is_variance_samp =
      function == exec::CanonicalAggregateFunction::variance_samp;
  const bool is_statistical =
      is_stddev_pop || is_variance_pop || is_stddev || is_variance ||
      is_stddev_samp || is_variance_samp;
  const bool is_regr_count =
      function == exec::CanonicalAggregateFunction::regr_count;
  const bool is_pair_statistical =
      function == exec::CanonicalAggregateFunction::corr ||
      function == exec::CanonicalAggregateFunction::covar_pop ||
      function == exec::CanonicalAggregateFunction::covar_samp ||
      is_regr_count ||
      function == exec::CanonicalAggregateFunction::regr_avgx ||
      function == exec::CanonicalAggregateFunction::regr_avgy ||
      function == exec::CanonicalAggregateFunction::regr_intercept ||
      function == exec::CanonicalAggregateFunction::regr_r2 ||
      function == exec::CanonicalAggregateFunction::regr_slope ||
      function == exec::CanonicalAggregateFunction::regr_sxx ||
      function == exec::CanonicalAggregateFunction::regr_sxy ||
      function == exec::CanonicalAggregateFunction::regr_syy;
  const bool is_string_agg =
      function == exec::CanonicalAggregateFunction::string_agg;
  const auto string_aggregate_profile =
      MatchLiveStringAggregateExpressionProfile(root.semantic_variant_id);
  const bool is_ordered_string_agg =
      is_string_agg && string_aggregate_profile.matched &&
      string_aggregate_profile.ordered;
  const bool is_listagg =
      function == exec::CanonicalAggregateFunction::listagg;
  const auto listagg_profile =
      MatchLiveListaggExpressionProfile(root.semantic_variant_id);
  const bool is_listagg_profile = is_listagg && listagg_profile.matched;
  const auto ordered_single_collection_profile =
      MatchLiveOrderedSingleCollectionExpressionProfile(
          root.semantic_variant_id);
  const bool is_array_agg =
      function == exec::CanonicalAggregateFunction::array_agg &&
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function == function;
  const bool is_json_agg =
      function == exec::CanonicalAggregateFunction::json_agg &&
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function == function;
  const auto json_object_aggregate_profile =
      MatchLiveJsonObjectAggregateExpressionProfile(
          root.semantic_variant_id);
  const bool is_json_object_agg =
      function == exec::CanonicalAggregateFunction::json_object_agg &&
      json_object_aggregate_profile.matched;
  const bool is_ordered_single_collection = is_array_agg || is_json_agg;
  const bool is_ordered_collection =
      is_ordered_single_collection || is_json_object_agg;
  const bool is_hypothetical_rank =
      function == exec::CanonicalAggregateFunction::rank;
  const bool is_hypothetical_dense_rank =
      function == exec::CanonicalAggregateFunction::dense_rank;
  const bool is_hypothetical_percent_rank =
      function == exec::CanonicalAggregateFunction::percent_rank;
  const bool is_hypothetical_cume_dist =
      function == exec::CanonicalAggregateFunction::cume_dist;
  const bool is_hypothetical =
      is_hypothetical_rank || is_hypothetical_dense_rank ||
      is_hypothetical_percent_rank || is_hypothetical_cume_dist;
  const bool is_mode = function == exec::CanonicalAggregateFunction::mode;
  const bool is_percentile_cont =
      function == exec::CanonicalAggregateFunction::percentile_cont;
  const bool is_percentile_disc =
      function == exec::CanonicalAggregateFunction::percentile_disc;
  const bool is_exact_percentile = is_percentile_cont || is_percentile_disc;
  const bool is_approx_count_distinct =
      function == exec::CanonicalAggregateFunction::approx_count_distinct;
  const bool is_approx_median =
      function == exec::CanonicalAggregateFunction::approx_median;
  const bool is_approx_percentile_cont =
      function == exec::CanonicalAggregateFunction::approx_percentile_cont;
  const bool is_approx_percentile_disc =
      function == exec::CanonicalAggregateFunction::approx_percentile_disc;
  const bool is_approx_percentile =
      is_approx_percentile_cont || is_approx_percentile_disc;
  const bool is_approx_top_k =
      function == exec::CanonicalAggregateFunction::approx_top_k;
  const bool is_approximate =
      is_approx_count_distinct || is_approx_median ||
      is_approx_percentile || is_approx_top_k;
  const bool is_ordered_set =
      is_hypothetical || is_mode || is_exact_percentile ||
      is_approx_percentile;
  if ((!is_count && !is_sum && !is_avg && !is_min && !is_max &&
       !is_boolean && !is_statistical && !is_pair_statistical &&
       !is_string_agg && !is_listagg_profile && !is_ordered_collection &&
       !is_ordered_set && !is_approximate) ||
      (count_star && !is_count)) {
    result.detail = "global aggregate function profile is not admitted";
    return result;
  }
  if (root.output_descriptor_ids.size() != 1 ||
      root.bound_expression_ids.size() != 1 ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      std::ranges::find(input_node.output_descriptor_ids,
                        root.output_descriptor_ids.front()) !=
          input_node.output_descriptor_ids.end()) {
    result.detail =
        "global aggregate input or output descriptor coverage is unresolved";
    return result;
  }

  const api::RelationalOutputRecord* output = nullptr;
  for (const auto& candidate : dag.outputs) {
    if (candidate.relation_node_id != root.logical_node_id) continue;
    if (allow_sibling_outputs &&
        (candidate.ordinal != expected_output_ordinal ||
         candidate.descriptor_id != root.output_descriptor_ids.front() ||
         candidate.expression_id != root.bound_expression_ids.front())) {
      continue;
    }
    if (output != nullptr) {
      result.detail = "global aggregate requires exactly one bound output";
      return result;
    }
    output = &candidate;
  }
  if (output == nullptr || output->ordinal != expected_output_ordinal ||
      !output->visible ||
      output->output_name_utf8.empty() ||
      output->descriptor_id != root.output_descriptor_ids.front() ||
      output->expression_id != root.bound_expression_ids.front()) {
    result.detail = "global aggregate output lineage is not exact";
    return result;
  }

  const auto expression = std::ranges::find_if(
      dag.expressions, [&](const auto& candidate) {
        return candidate.expression_id == root.bound_expression_ids.front();
      });
  const auto descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& candidate) {
        return candidate.descriptor_id == root.output_descriptor_ids.front();
      });
  const auto* aggregate =
      exec::LookupCanonicalAggregateByFunctionV1(function);
  std::size_t expected_argument_count = 1;
  if (count_star) {
    expected_argument_count = 0;
  } else if (is_listagg_profile) {
    expected_argument_count = listagg_profile.base_argument_count +
                              (has_filter ? 1U : 0U);
  } else if (is_ordered_set) {
    expected_argument_count = (is_mode ? 1U : 2U) +
                              (has_filter ? 1U : 0U);
  } else if (is_approx_top_k) {
    expected_argument_count = 2U + (has_filter ? 1U : 0U);
  } else if (has_filter) {
    expected_argument_count = 2;
    if (is_pair_statistical) {
      expected_argument_count = 3;
    } else if (is_string_agg) {
      expected_argument_count = is_ordered_string_agg ? 4 : 3;
    } else if (is_ordered_single_collection) {
      expected_argument_count = 3;
    } else if (is_json_object_agg) {
      expected_argument_count = 4;
    }
  } else if (is_hypothetical || is_exact_percentile ||
             is_approx_percentile || is_approx_top_k) {
    expected_argument_count = 2;
  } else if (is_json_object_agg || is_ordered_string_agg) {
    expected_argument_count = 3;
  } else if (is_pair_statistical || is_string_agg ||
             is_ordered_single_collection) {
    expected_argument_count = 2;
  }
  if (expression == dag.expressions.end() ||
      descriptor == dag.descriptors.end() || aggregate == nullptr ||
      !aggregate->executable ||
      expression->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      expression->child_expression_ids.size() != expected_argument_count ||
      expression->result_descriptor_id != descriptor->descriptor_id ||
      !expression->function_uuid.has_value() ||
      *expression->function_uuid != aggregate->function_uuid ||
      expression->bound_name_uuid.has_value() ||
      expression->literal_kind.has_value() ||
      expression->operator_name.has_value() ||
      expression->literal_or_parameter_ref.has_value()) {
    result.detail =
        "global aggregate function identity or argument binding is invalid";
    return result;
  }

  if (!count_star) {
    CanonicalRelationalExpressionRuntime expression_runtime(dag);
    for (std::size_t argument_ordinal = 0;
         argument_ordinal < expression->child_expression_ids.size();
         ++argument_ordinal) {
      const auto child_expression_id =
          expression->child_expression_ids[argument_ordinal];
      const auto argument = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == child_expression_id;
          });
      if ((is_hypothetical || is_exact_percentile ||
           is_approx_percentile || is_approx_top_k) &&
          argument_ordinal == 0) {
        const auto direct_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind == api::RelationalLiteralKind::kNumeric &&
            !argument->operator_name.has_value() &&
            argument->literal_or_parameter_ref.has_value() &&
            direct_descriptor != dag.descriptors.end() &&
            direct_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !direct_descriptor->collation_uuid.has_value() &&
            !direct_descriptor->timezone_profile_id.has_value() &&
            !direct_descriptor->width.has_value() &&
            !direct_descriptor->precision.has_value() &&
            !direct_descriptor->scale.has_value() &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global ordered-set direct argument must be one standalone, "
              "unqualified, non-NULL canonical numeric literal";
          return result;
        }
        api::EngineTypedValue direct_argument;
        std::string direct_detail;
        const std::string_view direct_type =
            (is_exact_percentile || is_approx_percentile)
                ? std::string_view("real64")
                : std::string_view("int64");
        if (!expression_runtime.EvaluateForConsumer(
                child_expression_id, direct_type,
                api::EngineCanonicalExpressionConsumer::aggregate,
                &direct_argument, &direct_detail) ||
            direct_argument.state != api::EngineValueState::value ||
            direct_argument.is_null ||
            direct_argument.descriptor.canonical_type_name != direct_type) {
          if (is_exact_percentile || is_approx_percentile) {
            result.detail =
                "global percentile fraction must be a canonical real64 "
                "literal";
          } else if (is_approx_top_k) {
            result.detail =
                "global approximate top-k bound must be a canonical int64 "
                "literal";
          } else {
            result.detail =
                "global hypothetical-set direct argument must be a canonical "
                "int64 literal";
          }
          if (!direct_detail.empty()) result.detail += ": " + direct_detail;
          return result;
        }
        result.direct_arguments.push_back(std::move(direct_argument));
        continue;
      }
      if ((is_string_agg || is_listagg_profile) && argument_ordinal == 1) {
        const auto separator_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id ==
                         argument->result_descriptor_id;
            });
        api::EngineTypedValue separator;
        std::string separator_detail;
        if (argument == dag.expressions.end() ||
            argument->expression_kind !=
                api::RelationalExpressionKind::kLiteral ||
            !argument->child_expression_ids.empty() ||
            argument->bound_name_uuid.has_value() ||
            argument->function_uuid.has_value() ||
            !argument->literal_kind.has_value() ||
            argument->operator_name.has_value() ||
            !argument->literal_or_parameter_ref.has_value() ||
            separator_descriptor == dag.descriptors.end() ||
            separator_descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            separator_descriptor->collation_uuid.has_value() ||
            separator_descriptor->timezone_profile_id.has_value() ||
            separator_descriptor->width.has_value() ||
            separator_descriptor->precision.has_value() ||
            separator_descriptor->scale.has_value() ||
            argument->result_descriptor_id ==
                root.output_descriptor_ids.front() ||
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) !=
                input_node.output_descriptor_ids.end() ||
            !expression_runtime.EvaluateForConsumer(
                child_expression_id, "text",
                api::EngineCanonicalExpressionConsumer::aggregate,
                &separator, &separator_detail) ||
            separator.state != api::EngineValueState::value ||
            separator.is_null ||
            separator.descriptor.canonical_type_name != "text") {
          result.detail =
              "global STRING_AGG/LISTAGG separator must be one standalone, "
              "unqualified, non-NULL canonical text literal";
          if (!separator_detail.empty()) {
            result.detail += ": " + separator_detail;
          }
          return result;
        }
        result.aggregate_separator = separator.encoded_value;
        continue;
      }
      if (is_listagg_profile && argument_ordinal >= 3 &&
          argument_ordinal < listagg_profile.base_argument_count) {
        const auto option_descriptor = std::ranges::find_if(
            dag.descriptors, [&](const auto& candidate) {
              return argument != dag.expressions.end() &&
                     candidate.descriptor_id == argument->result_descriptor_id;
            });
        const bool exact_literal =
            argument != dag.expressions.end() &&
            argument->expression_kind ==
                api::RelationalExpressionKind::kLiteral &&
            argument->child_expression_ids.empty() &&
            !argument->bound_name_uuid.has_value() &&
            !argument->function_uuid.has_value() &&
            argument->literal_kind.has_value() &&
            !argument->operator_name.has_value() &&
            argument->literal_or_parameter_ref.has_value() &&
            option_descriptor != dag.descriptors.end() &&
            option_descriptor->nullability ==
                api::RelationalNullability::kNonNull &&
            !option_descriptor->collation_uuid.has_value() &&
            !option_descriptor->timezone_profile_id.has_value() &&
            !option_descriptor->width.has_value() &&
            !option_descriptor->precision.has_value() &&
            !option_descriptor->scale.has_value() &&
            argument->result_descriptor_id !=
                root.output_descriptor_ids.front() &&
            std::ranges::find(input_node.output_descriptor_ids,
                              argument->result_descriptor_id) ==
                input_node.output_descriptor_ids.end();
        if (!exact_literal) {
          result.detail =
              "global LISTAGG overflow options must be standalone, "
              "unqualified, non-NULL canonical literals";
          return result;
        }
        api::EngineTypedValue option;
        std::string option_detail;
        if (argument_ordinal == 3) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "int64",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "int64") {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          std::int64_t decoded = 0;
          const auto [end, error] = std::from_chars(
              option.encoded_value.data(),
              option.encoded_value.data() + option.encoded_value.size(),
              decoded);
          if (error != std::errc{} ||
              end != option.encoded_value.data() + option.encoded_value.size() ||
              decoded <= 0 ||
              static_cast<std::uint64_t>(decoded) >
                  std::numeric_limits<std::size_t>::max()) {
            result.detail =
                "global LISTAGG overflow bound must be a positive canonical "
                "int64 literal";
            return result;
          }
          result.listagg_max_output_bytes =
              static_cast<std::size_t>(decoded);
          continue;
        }
        if (argument_ordinal == 4) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "text",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "text") {
            result.detail =
                "global LISTAGG truncation indicator must be a canonical "
                "text literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_truncation_indicator = option.encoded_value;
          continue;
        }
        if (argument_ordinal == 5) {
          if (!expression_runtime.EvaluateForConsumer(
                  child_expression_id, "boolean",
                  api::EngineCanonicalExpressionConsumer::aggregate,
                  &option, &option_detail) ||
              option.state != api::EngineValueState::value || option.is_null ||
              option.descriptor.canonical_type_name != "boolean" ||
              (option.encoded_value != "true" &&
               option.encoded_value != "false")) {
            result.detail =
                "global LISTAGG WITH/WITHOUT COUNT option must be a canonical "
                "boolean literal";
            if (!option_detail.empty()) result.detail += ": " + option_detail;
            return result;
          }
          result.listagg_with_count = option.encoded_value == "true";
          continue;
        }
      }
      if (argument == dag.expressions.end() ||
          argument->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          !argument->child_expression_ids.empty() ||
          !argument->bound_name_uuid.has_value() ||
          argument->function_uuid.has_value() ||
          argument->literal_kind.has_value() ||
          argument->operator_name.has_value() ||
          argument->literal_or_parameter_ref.has_value()) {
        result.detail =
            "global aggregate expression arguments are not exact bound input "
            "identifiers";
        return result;
      }
      const auto input_descriptor = std::ranges::find(
          input_node.output_descriptor_ids, argument->result_descriptor_id);
      if (input_descriptor == input_node.output_descriptor_ids.end() ||
          std::ranges::count(input_node.output_descriptor_ids,
                             argument->result_descriptor_id) != 1) {
        result.detail =
            "global aggregate expression descriptor is not uniquely supplied "
            "by its input";
        return result;
      }
      const auto value_column = static_cast<std::size_t>(
          std::distance(input_node.output_descriptor_ids.begin(),
                        input_descriptor));
      if (value_column >= input.batch.columns.size() ||
          input.batch.columns[value_column].descriptor_id !=
              argument->result_descriptor_id) {
        result.detail =
            "global aggregate expression input ordinal is not "
            "descriptor-exact";
        return result;
      }
      const auto input_type =
          input.batch.columns[value_column].descriptor.canonical_type_name;
      std::size_t filter_argument_ordinal = 1;
      if (is_pair_statistical) {
        filter_argument_ordinal = 2;
      } else if (is_string_agg) {
        filter_argument_ordinal = is_ordered_string_agg ? 3 : 2;
      } else if (is_ordered_single_collection) {
        filter_argument_ordinal = 2;
      } else if (is_json_object_agg) {
        filter_argument_ordinal = 3;
      } else if (is_listagg_profile) {
        filter_argument_ordinal = listagg_profile.base_argument_count;
      } else if (is_ordered_set) {
        filter_argument_ordinal = is_mode ? 1U : 2U;
      } else if (is_approx_top_k) {
        filter_argument_ordinal = 2U;
      }
      const bool is_filter_argument =
          has_filter && argument_ordinal == filter_argument_ordinal;
      if (is_filter_argument) {
        if (input_type != "boolean") {
          result.detail =
              "global aggregate FILTER input must be a canonical boolean "
              "column";
          return result;
        }
        std::vector<api::EngineSqlTruthValue> filter_truth_values;
        if (!MaterializeAggregateFilterTruthValues(
                input.batch, value_column, argument->result_descriptor_id,
                &filter_truth_values, &result.detail)) {
          return result;
        }
        result.filter_column = value_column;
        result.filter_descriptor_id = argument->result_descriptor_id;
        continue;
      }
      const bool is_order_argument =
          (is_ordered_string_agg && argument_ordinal == 2) ||
          (is_listagg_profile && argument_ordinal == 2) ||
          (is_ordered_single_collection && argument_ordinal == 1) ||
          (is_json_object_agg && argument_ordinal == 2) ||
          (is_mode && argument_ordinal == 0) ||
          ((is_hypothetical || is_exact_percentile ||
            is_approx_percentile) &&
           argument_ordinal == 1);
      if (is_order_argument) {
        if (input_type != "int64") {
          result.detail = is_ordered_set
                              ? "global ordered-set value/order input must be "
                                "a canonical int64 column"
                              : "global aggregate order input must be a "
                                "canonical int64 column";
          return result;
        }
        exec::CanonicalDescriptorOrderTerm order_term;
        order_term.column = value_column;
        order_term.expression_descriptor_id = argument->result_descriptor_id;
        order_term.direction =
            exec::CanonicalDescriptorOrderDirection::ascending;
        order_term.null_placement =
            exec::CanonicalDescriptorNullPlacement::last;
        const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
            order_term, input.batch.columns[value_column]);
        if (!validation.ok) {
          result.detail = validation.detail;
          return result;
        }
        result.aggregate_order_terms.push_back(std::move(order_term));
        if (is_ordered_set) {
          result.value_columns.push_back(value_column);
          result.value_descriptor_ids.push_back(
              argument->result_descriptor_id);
        }
        continue;
      }
      result.value_columns.push_back(value_column);
      result.value_descriptor_ids.push_back(argument->result_descriptor_id);
      if ((is_sum || is_avg || is_min || is_max || is_statistical ||
           is_pair_statistical) &&
          input_type != "int64") {
        if (is_sum) {
          result.detail = "global SUM input must be a canonical int64 column";
        } else if (is_avg) {
          result.detail = "global AVG input must be a canonical int64 column";
        } else if (is_statistical) {
          result.detail =
              "global unary statistical input must be a canonical int64 "
              "column";
        } else if (is_pair_statistical) {
          result.detail =
              "global pair statistical inputs must be canonical int64 "
              "columns";
        } else {
          result.detail =
              "global MIN/MAX input must be a canonical int64 column";
        }
        return result;
      }
      if (is_boolean && input_type != "boolean") {
        result.detail =
            "global BOOL_AND/BOOL_OR/EVERY input must be a canonical boolean "
            "column";
        return result;
      }
      if (is_string_agg && input_type != "text") {
        result.detail =
            "global STRING_AGG input must be a canonical text column";
        return result;
      }
      if (is_listagg_profile && input_type != "text") {
        result.detail =
            "global LISTAGG input must be a canonical text column";
        return result;
      }
      if (is_ordered_single_collection && input_type != "text") {
        result.detail =
            "global ARRAY_AGG/JSON_AGG input must be a canonical text column";
        return result;
      }
      if (is_json_object_agg && argument_ordinal == 0 &&
          input_type != "text") {
        result.detail =
            "global JSON_OBJECT_AGG key must be a canonical text column";
        return result;
      }
      if (is_json_object_agg && argument_ordinal == 1 &&
          input_type != "int64") {
        result.detail =
            "global JSON_OBJECT_AGG value must be a canonical int64 column";
        return result;
      }
      if (is_approx_median && input_type != "int64") {
        result.detail =
            "global APPROX_MEDIAN input must be a canonical int64 column";
        return result;
      }
      if ((is_approx_count_distinct || is_approx_top_k) &&
          input_type != "text") {
        result.detail =
            "global APPROX_COUNT_DISTINCT/APPROX_TOP_K input must be a "
            "canonical text column";
        return result;
      }
    }
  }
  const bool result_nullable =
      is_sum || is_avg || is_min || is_max || is_boolean || is_statistical ||
      (is_pair_statistical && !is_regr_count) || is_string_agg ||
      is_listagg_profile || is_ordered_collection || is_mode ||
      is_exact_percentile || is_approx_median || is_approx_percentile ||
      is_approx_top_k;
  const auto expected_nullability =
      result_nullable ? api::RelationalNullability::kNullable
                      : api::RelationalNullability::kNonNull;
  if (descriptor->nullability != expected_nullability ||
      descriptor->collation_uuid.has_value() ||
      descriptor->timezone_profile_id.has_value() ||
      descriptor->width.has_value() || descriptor->precision.has_value() ||
      descriptor->scale.has_value()) {
    if (is_sum) {
      result.detail =
          "global SUM result must be an unqualified nullable int64";
    } else if (is_avg) {
      result.detail =
          "global AVG result must be an unqualified nullable real64";
    } else if (is_min || is_max) {
      result.detail =
          "global MIN/MAX result must be an unqualified nullable int64";
    } else if (is_boolean) {
      result.detail =
          "global BOOL_AND/BOOL_OR/EVERY result must be an unqualified "
          "nullable boolean";
    } else if (is_statistical) {
      result.detail =
          "global unary statistical result must be an unqualified nullable "
          "real64";
    } else if (is_pair_statistical) {
      result.detail =
          is_regr_count
              ? "global REGR_COUNT result must be an unqualified non-null "
                "int64"
              : "global pair statistical result must be an unqualified "
                "nullable real64";
    } else if (is_string_agg) {
      result.detail =
          "global STRING_AGG result must be an unqualified nullable text";
    } else if (is_listagg_profile) {
      result.detail =
          "global LISTAGG result must be an unqualified nullable text";
    } else if (is_array_agg) {
      result.detail =
          "global ARRAY_AGG result must be an unqualified nullable "
          "list<text nullable>";
    } else if (is_json_agg) {
      result.detail =
          "global JSON_AGG result must be an unqualified nullable json";
    } else if (is_json_object_agg) {
      result.detail =
          "global JSON_OBJECT_AGG result must be an unqualified nullable "
          "json";
    } else if (is_mode) {
      result.detail =
          "global MODE result must be an unqualified nullable int64";
    } else if (is_exact_percentile) {
      result.detail =
          "global exact percentile result must be an unqualified nullable "
          "real64";
    } else if (is_approx_median || is_approx_percentile) {
      result.detail =
          "global approximate quantile result must be an unqualified "
          "nullable real64";
    } else if (is_approx_top_k) {
      result.detail =
          "global APPROX_TOP_K result must be an unqualified nullable json";
    } else if (is_approx_count_distinct) {
      result.detail =
          "global APPROX_COUNT_DISTINCT result must be an unqualified "
          "non-null int64";
    } else if (is_hypothetical_rank || is_hypothetical_dense_rank) {
      result.detail =
          "global hypothetical RANK/DENSE_RANK result must be an unqualified "
          "non-null int64";
    } else if (is_hypothetical_percent_rank ||
               is_hypothetical_cume_dist) {
      result.detail =
          "global hypothetical PERCENT_RANK/CUME_DIST result must be an "
          "unqualified non-null real64";
    } else {
      result.detail =
          "global COUNT result must be an unqualified non-null int64";
    }
    return result;
  }

  result.aggregate_descriptor =
      {aggregate->abi_version, aggregate->function, aggregate->builtin_id,
       aggregate->function_uuid, count_star};
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_uuid.canonical = descriptor->descriptor_uuid;
  engine_descriptor.descriptor_kind = "scalar";
  if (is_array_agg) {
    engine_descriptor.canonical_type_name = "list<text nullable>";
  } else if (is_json_agg || is_json_object_agg || is_approx_top_k) {
    engine_descriptor.canonical_type_name = "json";
  } else if (is_string_agg || is_listagg_profile) {
    engine_descriptor.canonical_type_name = "text";
  } else if (is_avg || is_statistical ||
             (is_pair_statistical && !is_regr_count) ||
             is_exact_percentile || is_approx_median ||
             is_approx_percentile || is_hypothetical_percent_rank ||
             is_hypothetical_cume_dist) {
    engine_descriptor.canonical_type_name = "real64";
  } else if (is_boolean) {
    engine_descriptor.canonical_type_name = "boolean";
  } else {
    engine_descriptor.canonical_type_name = "int64";
  }
  engine_descriptor.encoded_descriptor =
      "type_uuid=" + descriptor->type_uuid + ";nullability=" +
      (result_nullable ? "nullable" : "non_null");
  result.result_column = {output->output_name_utf8, engine_descriptor,
                          result_nullable, descriptor->descriptor_id};
  exec::CanonicalResultColumnBinding binding;
  binding.physical_column_ordinal = expected_output_ordinal;
  binding.visible = true;
  binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      expected_output_ordinal,
      output->output_name_utf8,
      descriptor->descriptor_uuid,
      descriptor->type_uuid,
      result_nullable ? exec::CanonicalResultNullability::kNullable
                      : exec::CanonicalResultNullability::kNonNull,
      std::nullopt,
      std::nullopt};
  result.result_bindings.push_back(std::move(binding));
  if (is_listagg_profile) {
    result.listagg_overflow_mode = listagg_profile.overflow_mode;
  }
  result.ok = true;
  return result;
}

PreparedGroupedCountSumRoot PrepareGroupedCountSumRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const LiveGroupedCountSumProfile& profile) {
  PreparedGroupedCountSumRoot result;
  const auto grouping_projection_count =
      profile.projects_grouping_metadata ? profile.key_count + 1 : 0;
  const auto expected_output_count =
      profile.key_count + 2 + grouping_projection_count;
  if (!profile.matched || profile.key_count == 0 ||
      (!profile.grouping_sets_from_sblr &&
       profile.expansion_kind ==
           exec::CanonicalAggregateGroupingExpansionKind::explicit_sets &&
       profile.grouping_sets.empty()) ||
      (profile.grouping_sets_from_sblr && !profile.grouping_sets.empty()) ||
      root.output_descriptor_ids.size() != expected_output_count ||
      root.bound_expression_ids.size() != expected_output_count ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      std::ranges::any_of(root.output_descriptor_ids,
                          [&](const auto descriptor_id) {
                            return std::ranges::count(
                                       root.output_descriptor_ids,
                                       descriptor_id) != 1;
                          }) ||
      root.output_descriptor_ids[profile.key_count] ==
          root.output_descriptor_ids[profile.key_count + 1]) {
    result.detail =
        "grouped COUNT/SUM shape does not match its exact key profile";
    return result;
  }

  std::vector<exec::CanonicalAggregateGroupingSet> explicit_grouping_sets;
  if (profile.grouping_sets_from_sblr) {
    std::vector<const api::RelationalGroupingSetRecord*> grouping_sets;
    for (const auto& grouping_set : dag.grouping_sets) {
      if (grouping_set.relation_node_id == root.logical_node_id) {
        grouping_sets.push_back(&grouping_set);
      }
    }
    std::ranges::sort(grouping_sets, {},
                      &api::RelationalGroupingSetRecord::ordinal);
    for (std::size_t ordinal = 0; ordinal < grouping_sets.size(); ++ordinal) {
      if (grouping_sets[ordinal]->ordinal != ordinal) {
        result.detail =
            "grouped COUNT/SUM grouping-set ordinals are not dense";
        return result;
      }
      exec::CanonicalAggregateGroupingSet prepared;
      for (const auto expression_id : grouping_sets[ordinal]->expression_ids) {
        const auto key = std::ranges::find(
            root.bound_expression_ids.begin(),
            root.bound_expression_ids.begin() + profile.key_count,
            expression_id);
        if (key == root.bound_expression_ids.begin() + profile.key_count) {
          result.detail =
              "grouped COUNT/SUM grouping-set member is not a bound key";
          return result;
        }
        const auto key_ordinal = static_cast<std::size_t>(std::distance(
            root.bound_expression_ids.begin(), key));
        if (!prepared.key_term_ordinals.empty() &&
            key_ordinal <= prepared.key_term_ordinals.back()) {
          result.detail =
              "grouped COUNT/SUM grouping-set members are not in key order";
          return result;
        }
        prepared.key_term_ordinals.push_back(key_ordinal);
      }
      explicit_grouping_sets.push_back(std::move(prepared));
    }
  } else {
    if (std::ranges::any_of(
            dag.grouping_sets, [&](const auto& grouping_set) {
              return grouping_set.relation_node_id == root.logical_node_id;
            })) {
      result.detail =
          "grouped COUNT/SUM fixed grouping profile has an unexpected payload";
      return result;
    }
    explicit_grouping_sets = profile.grouping_sets;
  }
  exec::CanonicalAggregateGroupingExpansionRequest expansion_request;
  expansion_request.kind = profile.expansion_kind;
  expansion_request.group_key_count = profile.key_count;
  expansion_request.explicit_grouping_sets =
      std::move(explicit_grouping_sets);
  const auto expansion =
      exec::ExpandCanonicalAggregateGroupingSets(expansion_request);
  if (!expansion.diagnostic.ok) {
    result.detail = "grouped COUNT/SUM expansion: " +
                    expansion.diagnostic.detail;
    return result;
  }
  result.grouping_sets = expansion.grouping_sets;
  std::vector<bool> key_used(profile.key_count, false);
  for (const auto& grouping_set : result.grouping_sets) {
    for (const auto key_ordinal : grouping_set.key_term_ordinals) {
      key_used[key_ordinal] = true;
    }
  }
  if (std::ranges::find(key_used, false) != key_used.end()) {
    result.detail =
        "grouped COUNT/SUM grouping-set expansion has an unused key";
    return result;
  }
  for (std::size_t key_ordinal = 0; key_ordinal < profile.key_count;
       ++key_ordinal) {
    const auto key_expression = std::ranges::find_if(
        dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id ==
                 root.bound_expression_ids[key_ordinal];
        });
    const auto key_descriptor = std::ranges::find_if(
        dag.descriptors, [&](const auto& candidate) {
          return key_expression != dag.expressions.end() &&
                 candidate.descriptor_id ==
                     key_expression->result_descriptor_id;
        });
    const auto key_output = std::ranges::find_if(
        dag.outputs, [&](const auto& candidate) {
          return key_expression != dag.expressions.end() &&
                 candidate.relation_node_id == root.logical_node_id &&
                 candidate.expression_id == key_expression->expression_id &&
                 candidate.descriptor_id ==
                     key_expression->result_descriptor_id;
        });
    if (key_expression == dag.expressions.end() ||
        key_descriptor == dag.descriptors.end() ||
        key_output == dag.outputs.end() ||
        key_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !key_expression->child_expression_ids.empty() ||
        !key_expression->bound_name_uuid.has_value() ||
        key_expression->function_uuid.has_value() ||
        key_expression->literal_kind.has_value() ||
        key_expression->operator_name.has_value() ||
        key_expression->literal_or_parameter_ref.has_value() ||
        root.output_descriptor_ids[key_ordinal] !=
            key_expression->result_descriptor_id ||
        key_output->ordinal != key_ordinal || !key_output->visible ||
        key_output->output_name_utf8.empty()) {
      result.detail =
          "grouped COUNT/SUM key lineage is not an exact bound identifier";
      return result;
    }
    const auto supplied_key = std::ranges::find(
        input_node.output_descriptor_ids,
        key_expression->result_descriptor_id);
    if (supplied_key == input_node.output_descriptor_ids.end() ||
        std::ranges::count(input_node.output_descriptor_ids,
                           key_expression->result_descriptor_id) != 1 ||
        std::ranges::count(root.output_descriptor_ids,
                           key_expression->result_descriptor_id) != 1) {
      result.detail =
          "grouped COUNT/SUM key descriptor is not uniquely bound";
      return result;
    }
    const auto key_column = static_cast<std::size_t>(std::distance(
        input_node.output_descriptor_ids.begin(), supplied_key));
    if (key_column >= input.batch.columns.size() ||
        input.batch.columns[key_column].descriptor_id !=
            key_expression->result_descriptor_id ||
        input.batch.columns[key_column].descriptor.canonical_type_name !=
            "int64") {
      result.detail =
          "grouped COUNT/SUM key must be a descriptor-exact int64 column";
      return result;
    }

    exec::CanonicalDescriptorOrderTerm key_term;
    key_term.column = key_column;
    key_term.expression_descriptor_id = key_expression->result_descriptor_id;
    key_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    key_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
    const auto key_validation = exec::ValidateCanonicalDescriptorOrderTerm(
        key_term, input.batch.columns[key_column]);
    if (!key_validation.ok) {
      result.detail = key_validation.detail;
      return result;
    }
    result.key_terms.push_back(std::move(key_term));

    auto key_result_column = input.batch.columns[key_column];
    key_result_column.stable_name = key_output->output_name_utf8;
    const bool key_can_be_omitted = std::ranges::any_of(
        result.grouping_sets, [&](const auto& grouping_set) {
          return std::ranges::find(grouping_set.key_term_ordinals,
                                   key_ordinal) ==
                 grouping_set.key_term_ordinals.end();
        });
    if (key_can_be_omitted && !key_result_column.nullable) {
      result.detail =
          "grouped COUNT/SUM grouping-set key result must admit grouping NULL";
      return result;
    }
    result.key_result_columns.push_back(std::move(key_result_column));

    exec::CanonicalResultColumnBinding key_binding;
    key_binding.physical_column_ordinal = key_ordinal;
    key_binding.visible = true;
    key_binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(key_ordinal),
        key_output->output_name_utf8,
        key_descriptor->descriptor_uuid,
        key_descriptor->type_uuid,
        ResultNullability(key_descriptor->nullability),
        key_descriptor->collation_uuid,
        key_descriptor->timezone_profile_id};
    result.result_bindings.push_back(std::move(key_binding));
  }

  auto count_root = root;
  count_root.output_descriptor_ids = {
      root.output_descriptor_ids[profile.key_count]};
  count_root.bound_expression_ids = {
      root.bound_expression_ids[profile.key_count]};
  result.count = PrepareGlobalAggregateRoot(
      dag, count_root, input_node, input,
      exec::CanonicalAggregateFunction::count, true, false, false,
      static_cast<std::uint32_t>(profile.key_count), true);
  if (!result.count.ok) {
    result.detail = "grouped COUNT: " + result.count.detail;
    return result;
  }

  auto sum_root = root;
  sum_root.output_descriptor_ids = {
      root.output_descriptor_ids[profile.key_count + 1]};
  sum_root.bound_expression_ids = {
      root.bound_expression_ids[profile.key_count + 1]};
  result.sum = PrepareGlobalAggregateRoot(
      dag, sum_root, input_node, input,
      exec::CanonicalAggregateFunction::sum, false, false, false,
      static_cast<std::uint32_t>(profile.key_count + 1), true);
  if (!result.sum.ok) {
    result.detail = "grouped SUM: " + result.sum.detail;
    return result;
  }

  result.result_bindings.push_back(result.count.result_bindings.front());
  result.result_bindings.push_back(result.sum.result_bindings.front());

  if (profile.projects_grouping_metadata) {
    const auto prepare_projection =
        [&](const std::size_t projection_ordinal,
            const api::RelationalExpressionKind expected_kind,
            const std::string_view expected_operator,
            const std::vector<std::uint32_t>& expected_children) {
          const auto expression_id =
              root.bound_expression_ids[projection_ordinal];
          const auto descriptor_id =
              root.output_descriptor_ids[projection_ordinal];
          const auto expression = std::ranges::find_if(
              dag.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          const auto descriptor = std::ranges::find_if(
              dag.descriptors, [&](const auto& candidate) {
                return candidate.descriptor_id == descriptor_id;
              });
          const auto output = std::ranges::find_if(
              dag.outputs, [&](const auto& candidate) {
                return candidate.relation_node_id == root.logical_node_id &&
                       candidate.expression_id == expression_id &&
                       candidate.descriptor_id == descriptor_id;
              });
          if (expression == dag.expressions.end() ||
              descriptor == dag.descriptors.end() ||
              output == dag.outputs.end() ||
              expression->expression_kind != expected_kind ||
              expression->child_expression_ids != expected_children ||
              expression->result_descriptor_id != descriptor_id ||
              expression->function_uuid.has_value() ||
              expression->bound_name_uuid.has_value() ||
              expression->literal_kind.has_value() ||
              expression->operator_name != expected_operator ||
              expression->literal_or_parameter_ref.has_value() ||
              descriptor->nullability !=
                  api::RelationalNullability::kNonNull ||
              descriptor->collation_uuid.has_value() ||
              descriptor->timezone_profile_id.has_value() ||
              descriptor->width.has_value() ||
              descriptor->precision.has_value() ||
              descriptor->scale.has_value() ||
              output->ordinal != projection_ordinal || !output->visible ||
              output->output_name_utf8.empty() ||
              std::ranges::count_if(
                  dag.outputs, [&](const auto& candidate) {
                    return candidate.relation_node_id ==
                               root.logical_node_id &&
                           candidate.expression_id == expression_id &&
                           candidate.descriptor_id == descriptor_id;
                  }) != 1) {
            return false;
          }

          api::EngineDescriptor engine_descriptor;
          engine_descriptor.descriptor_uuid.canonical =
              descriptor->descriptor_uuid;
          engine_descriptor.descriptor_kind = "scalar";
          engine_descriptor.canonical_type_name = "int64";
          engine_descriptor.encoded_descriptor =
              "type_uuid=" + descriptor->type_uuid +
              ";nullability=non_null";
          result.grouping_projection_columns.push_back(
              {output->output_name_utf8, engine_descriptor, false,
               descriptor_id});

          exec::CanonicalResultColumnBinding binding;
          binding.physical_column_ordinal = projection_ordinal;
          binding.visible = true;
          binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  static_cast<std::uint32_t>(projection_ordinal),
                  output->output_name_utf8,
                  descriptor->descriptor_uuid,
                  descriptor->type_uuid,
                  exec::CanonicalResultNullability::kNonNull,
                  std::nullopt,
                  std::nullopt};
          result.result_bindings.push_back(std::move(binding));
          return true;
        };

    const auto projection_begin = profile.key_count + 2;
    for (std::size_t key_ordinal = 0; key_ordinal < profile.key_count;
         ++key_ordinal) {
      if (!prepare_projection(
              projection_begin + key_ordinal,
              api::RelationalExpressionKind::kUnary, "grouping",
              {root.bound_expression_ids[key_ordinal]})) {
        result.detail =
            "GROUPING projection is not an exact bound-key special form";
        return result;
      }
    }
    std::vector<std::uint32_t> grouping_id_children;
    grouping_id_children.insert(
        grouping_id_children.end(), root.bound_expression_ids.begin(),
        root.bound_expression_ids.begin() +
            static_cast<std::ptrdiff_t>(profile.key_count));
    if (!prepare_projection(
            projection_begin + profile.key_count,
            api::RelationalExpressionKind::kBinary, "grouping_id",
            grouping_id_children)) {
      result.detail =
          "GROUPING_ID projection is not an exact ordered-key special form";
      return result;
    }
  }
  result.ok = true;
  return result;
}

// QOW-SOURCE-QRY-001-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-NOT-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-SUM-OR-GT-LIVE-V1
// QOW-SOURCE-QRY-001-GROUPING-SETS-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
// QOW-SOURCE-QRY-001-TWO-KEY-HAVING-NOT-COUNT-GT-LIVE-V1
PreparedGroupedHavingRoot PrepareGroupedHavingRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& filter_root,
    const plan::CanonicalLogicalRelationalNode& aggregate_root,
    const plan::CanonicalLogicalRelationalNode& values_node,
    const PreparedGroupedCountSumRoot& prepared_aggregate) {
  PreparedGroupedHavingRoot result;
  const auto* sum_registry_entry =
      exec::LookupCanonicalAggregateByFunctionV1(
          exec::CanonicalAggregateFunction::sum);
  const auto* count_registry_entry =
      exec::LookupCanonicalAggregateByFunctionV1(
          exec::CanonicalAggregateFunction::count);
  if (sum_registry_entry == nullptr || count_registry_entry == nullptr ||
      !sum_registry_entry->executable || !count_registry_entry->executable) {
    result.detail = "canonical COUNT/SUM aggregate registry is unavailable";
    return result;
  }
  const bool simple_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-sum-gt-int64-literal.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-SUM-GT-V1
  const bool not_not_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-sum-gt-int64-literal.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-COUNT-GT-V1
  const bool not_not_count_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-gt-int64-literal.v1";
  const bool not_not_count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-sum-and-gt-int64-literals.v1";
  const bool not_not_count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-count-sum-or-gt-int64-literals.v1";
  // QOW-SOURCE-QRY-001-ENGINE-TWO-KEY-HAVING-NOT-NOT-SUM-COUNT-OR-GT-LIVE-V1
  const bool not_not_sum_count_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-not-sum-count-or-gt-int64-literals.v1";
  const bool not_not_count_sum_boolean_profile =
      not_not_count_sum_and_profile || not_not_count_sum_or_profile ||
      not_not_sum_count_or_profile;
  const bool not_sum_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-sum-gt-int64-literal.v1";
  const bool not_count_profile =
      filter_root.semantic_variant_id ==
          "filter.having-not-count-gt-int64-literal.v1";
  const bool not_count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-count-sum-and-gt-int64-literals.v1";
  const bool not_count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-not-count-sum-or-gt-int64-literals.v1";
  const bool not_count_sum_boolean_profile =
      not_count_sum_and_profile || not_count_sum_or_profile;
  const bool count_sum_and_profile =
      filter_root.semantic_variant_id ==
      "filter.having-count-sum-and-gt-int64-literals.v1";
  const bool count_sum_or_profile =
      filter_root.semantic_variant_id ==
      "filter.having-count-sum-or-gt-int64-literals.v1";
  const bool count_sum_boolean_profile =
      count_sum_and_profile || count_sum_or_profile ||
      not_count_sum_boolean_profile || not_not_count_sum_boolean_profile;
  std::size_t key_count = 0;
  std::size_t grouping_projection_count = 0;
  if (aggregate_root.semantic_variant_id ==
      "aggregate.grouped-int64-key-count-sum.v1") {
    key_count = 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouped-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.rollup-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum.v1") {
    key_count = 2;
  } else if (aggregate_root.semantic_variant_id ==
             "aggregate.cube-int64-keys-count-sum-grouping.v1") {
    key_count = 2;
    grouping_projection_count = key_count + 1;
  } else {
    result.detail =
        "HAVING aggregate is not an admitted grouped COUNT/SUM profile";
    return result;
  }
  const auto expected_output_count =
      key_count + 2 + grouping_projection_count;
  const bool admitted_two_key_sum_profile =
      simple_sum_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1");
  const bool exact_ordinary_two_key_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_sum_profile =
      not_not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_profile =
      not_not_count_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_sum_and_profile =
      not_not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_count_sum_or_profile =
      not_not_count_sum_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_not_sum_count_or_profile =
      not_not_sum_count_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_profile =
      not_count_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_ordinary_two_key_not_count_sum_or_profile =
      not_count_sum_or_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouped-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 1 &&
      prepared_aggregate.grouping_sets.front().key_term_ordinals ==
          std::vector<std::size_t>{0, 1};
  const bool exact_grouping_sets_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  // QOW-SOURCE-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_grouping_sets_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  // QOW-SOURCE-QRY-001-ROLLUP-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_rollup_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_rollup_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-CUBE-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_cube_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  // QOW-SOURCE-QRY-001-CUBE-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-LIVE-V1
  const bool exact_cube_metadata_not_count_sum_and_profile =
      not_count_sum_and_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool admitted_not_count_sum_and_profile =
      exact_ordinary_two_key_not_count_sum_and_profile ||
      exact_grouping_sets_not_count_sum_and_profile ||
      exact_grouping_sets_metadata_not_count_sum_and_profile ||
      exact_rollup_not_count_sum_and_profile ||
      exact_rollup_metadata_not_count_sum_and_profile ||
      exact_cube_not_count_sum_and_profile ||
      exact_cube_metadata_not_count_sum_and_profile;
  const bool exact_grouping_sets_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_grouping_sets_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_rollup_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_rollup_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.rollup-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_cube_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool exact_cube_metadata_not_sum_profile =
      not_sum_profile &&
      aggregate_root.semantic_variant_id ==
          "aggregate.cube-int64-keys-count-sum-grouping.v1" &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();
  const bool admitted_not_sum_profile =
      exact_ordinary_two_key_not_sum_profile ||
      exact_grouping_sets_not_sum_profile ||
      exact_grouping_sets_metadata_not_sum_profile ||
      exact_rollup_not_sum_profile || exact_rollup_metadata_not_sum_profile ||
      exact_cube_not_sum_profile || exact_cube_metadata_not_sum_profile;
  const bool admitted_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-key-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouped-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1");
  const bool exact_grouping_sets_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals.empty() &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals ==
          prepared_aggregate.grouping_sets[0].key_term_ordinals;
  const bool exact_rollup_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.rollup-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 3 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals.empty();
  const bool exact_cube_or_profile =
      count_sum_or_profile &&
      (aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum.v1" ||
       aggregate_root.semantic_variant_id ==
           "aggregate.cube-int64-keys-count-sum-grouping.v1") &&
      prepared_aggregate.grouping_sets.size() == 4 &&
      prepared_aggregate.grouping_sets[0].key_term_ordinals ==
          std::vector<std::size_t>{0, 1} &&
      prepared_aggregate.grouping_sets[1].key_term_ordinals ==
          std::vector<std::size_t>{0} &&
      prepared_aggregate.grouping_sets[2].key_term_ordinals ==
          std::vector<std::size_t>{1} &&
      prepared_aggregate.grouping_sets[3].key_term_ordinals.empty();

  if (!prepared_aggregate.ok ||
      aggregate_root.output_descriptor_ids.size() != expected_output_count ||
      aggregate_root.bound_expression_ids.size() != expected_output_count ||
      (!simple_sum_profile && !not_not_sum_profile &&
       !not_not_count_profile && !not_not_count_sum_boolean_profile &&
       !not_sum_profile && !not_count_profile &&
       !count_sum_boolean_profile) ||
      (not_not_sum_profile &&
       !exact_ordinary_two_key_not_not_sum_profile) ||
      (not_not_count_profile &&
       !exact_ordinary_two_key_not_not_count_profile) ||
      (not_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_count_sum_and_profile) ||
      (not_not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_not_count_sum_or_profile) ||
      (not_not_sum_count_or_profile &&
       !exact_ordinary_two_key_not_not_sum_count_or_profile) ||
      (not_count_profile && !exact_ordinary_two_key_not_count_profile) ||
      (not_count_sum_and_profile && !admitted_not_count_sum_and_profile) ||
      (not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_count_sum_or_profile) ||
      (not_sum_profile && !admitted_not_sum_profile) ||
      (count_sum_or_profile && !admitted_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.grouping-sets-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1") &&
       !exact_grouping_sets_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.rollup-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.rollup-int64-keys-count-sum-grouping.v1") &&
       !exact_rollup_or_profile) ||
      (count_sum_or_profile &&
       (aggregate_root.semantic_variant_id ==
            "aggregate.cube-int64-keys-count-sum.v1" ||
        aggregate_root.semantic_variant_id ==
            "aggregate.cube-int64-keys-count-sum-grouping.v1") &&
       !exact_cube_or_profile) ||
      (key_count == 2 && !count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_sum_profile &&
       !exact_ordinary_two_key_not_not_count_profile &&
       !exact_ordinary_two_key_not_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_not_count_sum_or_profile &&
       !exact_ordinary_two_key_not_not_sum_count_or_profile &&
       !admitted_not_count_sum_and_profile &&
       !exact_ordinary_two_key_not_count_sum_or_profile &&
       !admitted_or_profile &&
       !admitted_two_key_sum_profile &&
       !admitted_not_sum_profile &&
       !exact_ordinary_two_key_not_count_profile) ||
      filter_root.input_logical_node_ids !=
          std::vector<std::uint32_t>{aggregate_root.logical_node_id} ||
      filter_root.output_descriptor_ids !=
          aggregate_root.output_descriptor_ids ||
      filter_root.bound_expression_ids.size() != 1 ||
      prepared_aggregate.key_terms.size() != key_count ||
      prepared_aggregate.result_bindings.size() != expected_output_count) {
    result.detail = "HAVING root does not preserve the grouped aggregate schema";
    return result;
  }

  const auto expression_by_id = [&](const std::uint32_t expression_id)
      -> const api::RelationalExpressionRecord* {
    const auto expression =
        std::ranges::find_if(dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id == expression_id;
        });
    return expression == dag.expressions.end() ? nullptr : &*expression;
  };
  if (exact_ordinary_two_key_not_not_sum_profile ||
      exact_ordinary_two_key_not_not_count_profile ||
      exact_ordinary_two_key_not_not_count_sum_and_profile ||
      exact_ordinary_two_key_not_not_count_sum_or_profile ||
      exact_ordinary_two_key_not_not_sum_count_or_profile ||
      exact_ordinary_two_key_not_count_profile ||
      exact_ordinary_two_key_not_count_sum_or_profile) {
    std::unordered_map<std::uint32_t,
                       const api::RelationalExpressionRecord*>
        expressions_by_id;
    expressions_by_id.reserve(dag.expressions.size());
    for (const auto& expression : dag.expressions) {
      expressions_by_id.emplace(expression.expression_id, &expression);
    }

    std::vector<std::uint32_t> pending_expression_ids;
    for (const auto& row : dag.values_rows) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    row.expression_ids.begin(),
                                    row.expression_ids.end());
    }
    for (const auto& node : dag.nodes) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    node.bound_expression_ids.begin(),
                                    node.bound_expression_ids.end());
    }
    for (const auto& output : dag.outputs) {
      pending_expression_ids.push_back(output.expression_id);
    }
    for (const auto& grouping_set : dag.grouping_sets) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    grouping_set.expression_ids.begin(),
                                    grouping_set.expression_ids.end());
    }
    for (const auto& property : dag.properties) {
      pending_expression_ids.insert(pending_expression_ids.end(),
                                    property.expression_ids.begin(),
                                    property.expression_ids.end());
      for (const auto& term : property.ordering_terms) {
        pending_expression_ids.push_back(term.expression_id);
      }
    }

    std::unordered_set<std::uint32_t> reachable_expression_ids;
    reachable_expression_ids.reserve(dag.expressions.size());
    while (!pending_expression_ids.empty()) {
      const auto expression_id = pending_expression_ids.back();
      pending_expression_ids.pop_back();
      if (!reachable_expression_ids.insert(expression_id).second) continue;
      const auto expression = expressions_by_id.find(expression_id);
      if (expression == expressions_by_id.end()) {
        result.detail =
            "bounded NOT profile contains an invalid expression owner";
        return result;
      }
      pending_expression_ids.insert(
          pending_expression_ids.end(),
          expression->second->child_expression_ids.begin(),
          expression->second->child_expression_ids.end());
    }
    if (expressions_by_id.size() != dag.expressions.size() ||
        reachable_expression_ids.size() != expressions_by_id.size()) {
      result.detail =
          "bounded NOT profile contains an unowned relational expression";
      return result;
    }
  }
  const auto descriptor_by_id = [&](const std::uint32_t descriptor_id)
      -> const api::RelationalTypeDescriptor* {
    const auto descriptor =
        std::ranges::find_if(dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == descriptor_id;
        });
    return descriptor == dag.descriptors.end() ? nullptr : &*descriptor;
  };
  const auto isolated_predicate_descriptor =
      [&](const api::RelationalExpressionRecord* expression) {
        if (expression == nullptr) return false;
        const auto* descriptor =
            descriptor_by_id(expression->result_descriptor_id);
        return descriptor != nullptr &&
               descriptor->nullability ==
                   api::RelationalNullability::kNullable &&
               !descriptor->collation_uuid.has_value() &&
               !descriptor->timezone_profile_id.has_value() &&
               !descriptor->width.has_value() &&
               !descriptor->precision.has_value() &&
               !descriptor->scale.has_value() &&
               std::ranges::find(filter_root.output_descriptor_ids,
                                 expression->result_descriptor_id) ==
                   filter_root.output_descriptor_ids.end();
      };
  const auto exact_binary_operator =
      [&](const api::RelationalExpressionRecord* expression,
          const std::string_view operator_name) {
        return expression != nullptr &&
               expression->expression_kind ==
                   api::RelationalExpressionKind::kBinary &&
               expression->child_expression_ids.size() == 2 &&
               expression->operator_name == operator_name &&
               !expression->function_uuid.has_value() &&
               !expression->bound_name_uuid.has_value() &&
               !expression->literal_kind.has_value() &&
               !expression->literal_or_parameter_ref.has_value() &&
               isolated_predicate_descriptor(expression);
      };
  const auto exact_unary_operator =
      [&](const api::RelationalExpressionRecord* expression,
          const std::string_view operator_name) {
        return expression != nullptr &&
               expression->expression_kind ==
                   api::RelationalExpressionKind::kUnary &&
               expression->child_expression_ids.size() == 1 &&
               expression->operator_name == operator_name &&
               !expression->function_uuid.has_value() &&
               !expression->bound_name_uuid.has_value() &&
               !expression->literal_kind.has_value() &&
               !expression->literal_or_parameter_ref.has_value() &&
               isolated_predicate_descriptor(expression);
      };

  std::vector<const api::RelationalOutputRecord*> aggregate_outputs;
  std::vector<const api::RelationalOutputRecord*> filter_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == aggregate_root.logical_node_id) {
      aggregate_outputs.push_back(&output);
    } else if (output.relation_node_id == filter_root.logical_node_id) {
      filter_outputs.push_back(&output);
    }
  }
  std::ranges::sort(aggregate_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  std::ranges::sort(filter_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  if (aggregate_outputs.size() != expected_output_count ||
      filter_outputs.size() != expected_output_count) {
    result.detail = "HAVING output lineage coverage is incomplete";
    return result;
  }
  if (exact_grouping_sets_metadata_not_count_sum_and_profile ||
      exact_rollup_metadata_not_count_sum_and_profile ||
      exact_cube_metadata_not_count_sum_and_profile ||
      exact_grouping_sets_metadata_not_sum_profile ||
      exact_rollup_metadata_not_sum_profile ||
      exact_cube_metadata_not_sum_profile) {
    constexpr std::array<std::string_view, 7> kOutputNames = {
        "key_a",      "key_b",     "row_count", "total_amount",
        "grouping_a", "grouping_b", "grouping_id"};
    for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
      if (aggregate_outputs[ordinal]->output_id != 4U + ordinal ||
          filter_outputs[ordinal]->output_id != 11U + ordinal ||
          aggregate_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          filter_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal]) {
        result.detail =
            "metadata NOT-SUM output identity is not exact";
        return result;
      }
    }
  }
  if ((exact_ordinary_two_key_not_not_sum_profile ||
       exact_ordinary_two_key_not_not_count_profile ||
       exact_ordinary_two_key_not_not_count_sum_and_profile ||
       exact_ordinary_two_key_not_not_count_sum_or_profile ||
       exact_ordinary_two_key_not_not_sum_count_or_profile ||
       admitted_not_count_sum_and_profile ||
       exact_ordinary_two_key_not_count_profile ||
       exact_ordinary_two_key_not_count_sum_or_profile) &&
      !exact_grouping_sets_metadata_not_count_sum_and_profile &&
      !exact_rollup_metadata_not_count_sum_and_profile &&
      !exact_cube_metadata_not_count_sum_and_profile) {
    constexpr std::array<std::string_view, 4> kOutputNames = {
        "key_a", "key_b", "row_count", "total_amount"};
    for (std::size_t ordinal = 0; ordinal < kOutputNames.size(); ++ordinal) {
      if (aggregate_outputs[ordinal]->output_id != 4U + ordinal ||
          filter_outputs[ordinal]->output_id != 8U + ordinal ||
          aggregate_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          filter_outputs[ordinal]->output_name_utf8 !=
              kOutputNames[ordinal] ||
          !aggregate_outputs[ordinal]->visible ||
          !filter_outputs[ordinal]->visible) {
        result.detail =
            "bounded NOT predicate output identity is not exact";
        return result;
      }
    }
  }
  for (std::size_t ordinal = 0; ordinal < filter_outputs.size(); ++ordinal) {
    const auto& aggregate_output = *aggregate_outputs[ordinal];
    const auto& filter_output = *filter_outputs[ordinal];
    if (aggregate_output.ordinal != ordinal || filter_output.ordinal != ordinal ||
        aggregate_output.expression_id !=
            aggregate_root.bound_expression_ids[ordinal] ||
        filter_output.expression_id != aggregate_output.expression_id ||
        filter_output.descriptor_id != aggregate_output.descriptor_id ||
        filter_output.descriptor_id !=
            filter_root.output_descriptor_ids[ordinal] ||
        filter_output.visible != aggregate_output.visible ||
        filter_output.output_name_utf8 != aggregate_output.output_name_utf8 ||
        filter_output.output_name_utf8.empty()) {
      result.detail = "HAVING output lineage does not exactly mirror its aggregate";
      return result;
    }
  }

  const auto* predicate =
      expression_by_id(filter_root.bound_expression_ids.front());
  const api::RelationalExpressionRecord* inner_not = nullptr;
  const api::RelationalExpressionRecord* boolean_root = nullptr;
  const api::RelationalExpressionRecord* count_comparison = nullptr;
  const api::RelationalExpressionRecord* sum_comparison = nullptr;
  if (simple_sum_profile && exact_binary_operator(predicate, ">")) {
    sum_comparison = predicate;
  } else if ((not_not_sum_profile || not_not_count_profile ||
              not_not_count_sum_boolean_profile) &&
             exact_unary_operator(predicate, "NOT")) {
    inner_not =
        expression_by_id(predicate->child_expression_ids.front());
    if (exact_unary_operator(inner_not, "NOT")) {
      if (not_not_count_sum_boolean_profile) {
        boolean_root =
            expression_by_id(inner_not->child_expression_ids.front());
        if (exact_binary_operator(
                boolean_root,
                (not_not_count_sum_or_profile ||
                 not_not_sum_count_or_profile)
                    ? "OR"
                    : "AND")) {
          if (not_not_sum_count_or_profile) {
            sum_comparison =
                expression_by_id(boolean_root->child_expression_ids[0]);
            count_comparison =
                expression_by_id(boolean_root->child_expression_ids[1]);
          } else {
            count_comparison =
                expression_by_id(boolean_root->child_expression_ids[0]);
            sum_comparison =
                expression_by_id(boolean_root->child_expression_ids[1]);
          }
        }
      } else if (not_not_count_profile) {
        count_comparison =
            expression_by_id(inner_not->child_expression_ids.front());
      } else {
        sum_comparison =
            expression_by_id(inner_not->child_expression_ids.front());
      }
    }
  } else if ((not_sum_profile || not_count_profile ||
              not_count_sum_boolean_profile) &&
             exact_unary_operator(predicate, "NOT")) {
    const auto* operand =
        expression_by_id(predicate->child_expression_ids.front());
    if (not_count_sum_boolean_profile &&
        exact_binary_operator(
            operand, not_count_sum_or_profile ? "OR" : "AND")) {
      boolean_root = operand;
      count_comparison =
          expression_by_id(operand->child_expression_ids[0]);
      sum_comparison =
          expression_by_id(operand->child_expression_ids[1]);
    } else if (not_count_profile) {
      count_comparison = operand;
    } else {
      sum_comparison = operand;
    }
  } else if (count_sum_boolean_profile &&
             exact_binary_operator(
                 predicate,
                 (count_sum_or_profile || not_count_sum_or_profile)
                     ? "OR"
                     : "AND")) {
    boolean_root = predicate;
    count_comparison = expression_by_id(predicate->child_expression_ids[0]);
    sum_comparison = expression_by_id(predicate->child_expression_ids[1]);
  }
  if ((!not_count_profile && !not_not_count_profile &&
       !exact_binary_operator(sum_comparison, ">")) ||
      (not_count_profile &&
       (!exact_binary_operator(count_comparison, ">") ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{count_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            count_comparison->result_descriptor_id)) ||
      (not_sum_profile &&
       predicate->result_descriptor_id !=
           sum_comparison->result_descriptor_id) ||
      (not_not_sum_profile &&
       (inner_not == nullptr ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{sum_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            sum_comparison->result_descriptor_id)) ||
      (not_not_count_profile &&
       (inner_not == nullptr ||
        !exact_binary_operator(count_comparison, ">") ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{count_comparison->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            count_comparison->result_descriptor_id)) ||
      (not_not_count_sum_boolean_profile &&
       (inner_not == nullptr || boolean_root == nullptr ||
        predicate->child_expression_ids !=
            std::vector<std::uint32_t>{inner_not->expression_id} ||
        inner_not->child_expression_ids !=
            std::vector<std::uint32_t>{boolean_root->expression_id} ||
        predicate->result_descriptor_id !=
            inner_not->result_descriptor_id ||
        inner_not->result_descriptor_id !=
            boolean_root->result_descriptor_id)) ||
      (count_sum_boolean_profile &&
       (boolean_root == nullptr ||
        !exact_binary_operator(count_comparison, ">") ||
        boolean_root->result_descriptor_id !=
            count_comparison->result_descriptor_id ||
        boolean_root->result_descriptor_id !=
            sum_comparison->result_descriptor_id ||
        (!not_count_sum_boolean_profile &&
         !not_not_count_sum_boolean_profile && boolean_root != predicate) ||
        (not_count_sum_boolean_profile &&
         (predicate->child_expression_ids !=
              std::vector<std::uint32_t>{boolean_root->expression_id} ||
          predicate->result_descriptor_id !=
              boolean_root->result_descriptor_id)) ||
        (not_not_count_sum_boolean_profile &&
         inner_not->result_descriptor_id !=
             boolean_root->result_descriptor_id)))) {
    result.detail =
        "HAVING predicate is not the exact admitted comparison or ordered Boolean profile";
    return result;
  }

  const api::RelationalExpressionRecord* having_sum = nullptr;
  const api::RelationalExpressionRecord* sum_threshold = nullptr;
  if (!not_count_profile && !not_not_count_profile) {
    having_sum = expression_by_id(sum_comparison->child_expression_ids[0]);
    sum_threshold = expression_by_id(sum_comparison->child_expression_ids[1]);
    const auto* projected_sum =
        expression_by_id(aggregate_root.bound_expression_ids[key_count + 1]);
    if (having_sum == nullptr || sum_threshold == nullptr ||
        projected_sum == nullptr ||
        having_sum->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        having_sum->function_uuid != sum_registry_entry->function_uuid ||
        having_sum->child_expression_ids.size() != 1 ||
        having_sum->result_descriptor_id !=
            aggregate_root.output_descriptor_ids[key_count + 1] ||
        having_sum->bound_name_uuid.has_value() ||
        having_sum->literal_kind.has_value() ||
        having_sum->operator_name.has_value() ||
        having_sum->literal_or_parameter_ref.has_value() ||
        projected_sum->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        projected_sum->function_uuid != having_sum->function_uuid ||
        projected_sum->child_expression_ids.size() != 1 ||
        projected_sum->result_descriptor_id !=
            having_sum->result_descriptor_id) {
      result.detail =
          "HAVING SUM identity does not match the projected aggregate state";
      return result;
    }

    const auto* having_argument =
        expression_by_id(having_sum->child_expression_ids.front());
    const auto* projected_argument =
        expression_by_id(projected_sum->child_expression_ids.front());
    if (having_argument == nullptr || projected_argument == nullptr ||
        having_argument->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        projected_argument->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        having_argument->result_descriptor_id !=
            projected_argument->result_descriptor_id ||
        having_argument->bound_name_uuid !=
            projected_argument->bound_name_uuid ||
        !having_argument->bound_name_uuid.has_value() ||
        std::ranges::find(values_node.output_descriptor_ids,
                          having_argument->result_descriptor_id) ==
            values_node.output_descriptor_ids.end()) {
      result.detail =
          "HAVING SUM argument does not bind the aggregate input column";
      return result;
    }
  }

  const auto prepare_threshold =
      [&](const api::RelationalExpressionRecord* threshold,
          const std::string_view label) {
        if (threshold == nullptr) return false;
        const auto* descriptor =
            descriptor_by_id(threshold->result_descriptor_id);
        if (threshold->expression_kind !=
                api::RelationalExpressionKind::kLiteral ||
            threshold->literal_kind !=
                api::RelationalLiteralKind::kNumeric ||
            !threshold->child_expression_ids.empty() ||
            threshold->function_uuid.has_value() ||
            threshold->bound_name_uuid.has_value() ||
            threshold->operator_name.has_value() ||
            !threshold->literal_or_parameter_ref.has_value() ||
            descriptor == nullptr ||
            descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            descriptor->collation_uuid.has_value() ||
            descriptor->timezone_profile_id.has_value() ||
            descriptor->width.has_value() || descriptor->precision.has_value() ||
            descriptor->scale.has_value() ||
            std::ranges::find(filter_root.output_descriptor_ids,
                              threshold->result_descriptor_id) !=
                filter_root.output_descriptor_ids.end()) {
          result.detail = std::string("HAVING ") + std::string(label) +
                          " threshold is not one standalone non-NULL numeric literal";
          return false;
        }
        CanonicalRelationalExpressionRuntime expression_runtime(dag);
        api::EngineTypedValue value;
        if (!expression_runtime.EvaluateForConsumer(
                threshold->expression_id, "int64",
                api::EngineCanonicalExpressionConsumer::aggregate, &value,
                &result.detail) ||
            value.state != api::EngineValueState::value || value.is_null ||
            value.descriptor.canonical_type_name != "int64") {
          if (result.detail.empty()) {
            result.detail = std::string("HAVING ") + std::string(label) +
                            " threshold is outside canonical int64";
          }
          return false;
        }
        std::int64_t exact = 0;
        const auto [end, error] = std::from_chars(
            value.encoded_value.data(),
            value.encoded_value.data() + value.encoded_value.size(), exact);
        if (error != std::errc{} ||
            end != value.encoded_value.data() + value.encoded_value.size()) {
          result.detail = std::string("HAVING ") + std::string(label) +
                          " threshold is outside exact int64 admission";
          return false;
        }
        return true;
      };
  if (!not_count_profile && !not_not_count_profile &&
      !prepare_threshold(sum_threshold, "SUM")) {
    return result;
  }

  const api::RelationalExpressionRecord* having_count = nullptr;
  if (count_sum_boolean_profile || not_count_profile ||
      not_not_count_profile) {
    having_count =
        expression_by_id(count_comparison->child_expression_ids[0]);
    const auto* count_threshold =
        expression_by_id(count_comparison->child_expression_ids[1]);
    const auto* projected_count =
        expression_by_id(aggregate_root.bound_expression_ids[key_count]);
    if (having_count == nullptr || projected_count == nullptr ||
        having_count->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        having_count->function_uuid != count_registry_entry->function_uuid ||
        !having_count->child_expression_ids.empty() ||
        having_count->result_descriptor_id !=
            aggregate_root.output_descriptor_ids[key_count] ||
        having_count->bound_name_uuid.has_value() ||
        having_count->literal_kind.has_value() ||
        having_count->operator_name.has_value() ||
        having_count->literal_or_parameter_ref.has_value() ||
        projected_count->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        projected_count->function_uuid != having_count->function_uuid ||
        !projected_count->child_expression_ids.empty() ||
        projected_count->result_descriptor_id !=
            having_count->result_descriptor_id) {
      result.detail =
          "HAVING COUNT identity does not match the projected aggregate state";
      return result;
    }
    if (!prepare_threshold(count_threshold, "COUNT")) {
      return result;
    }
  }
  result.predicate_expression_id = predicate->expression_id;
  result.row_binding.row_descriptor_ids =
      filter_root.output_descriptor_ids;
  if (having_sum != nullptr) {
    result.row_binding.slots.push_back(
        {having_sum->expression_id, having_sum->result_descriptor_id,
         key_count + 1,
         CanonicalRelationalExpressionRowSlotKind::materialized_function});
  }
  if (having_count != nullptr) {
    result.row_binding.slots.push_back(
        {having_count->expression_id, having_count->result_descriptor_id,
         key_count,
         CanonicalRelationalExpressionRowSlotKind::materialized_function});
  }
  result.output_column_count = expected_output_count;
  result.ok = true;
  return result;
}

bool BindTimezoneOrderAuthority(
    const api::EngineRequestContext& context,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  if (term == nullptr || detail == nullptr) return false;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  (void)context;
  *detail =
      "temporal ordering requires the production engine timezone catalog";
  return false;
#else
  const auto resolved = api::LookupEngineTimezoneSeedAuthority(context);
  if (!resolved.ok || !resolved.authority.active ||
      resolved.authority.resource_epoch == 0 ||
      resolved.authority.timezone_epoch == 0) {
    *detail = resolved.diagnostic.code.empty()
                  ? "temporal ordering lacks current engine timezone authority"
                  : resolved.diagnostic.code;
    return false;
  }
  term->resource_epoch = resolved.authority.resource_epoch;
  term->timezone_epoch = resolved.authority.timezone_epoch;
  term->timezone_seed.active = resolved.authority.active;
  term->timezone_seed.seed_pack_name = resolved.authority.seed_pack_name;
  term->timezone_seed.seed_pack_version =
      resolved.authority.seed_pack_version;
  term->timezone_seed.content_hash = resolved.authority.content_hash;
  term->timezone_seed.timezone_records =
      resolved.authority.timezone_records;
  term->timezone_seed.timezone_transition_records =
      resolved.authority.timezone_transition_records;
  term->timezone_seed.timezone_leap_second_records =
      resolved.authority.timezone_leap_second_records;
  term->timezone_seed.timezone_names = resolved.authority.timezone_names;
  return true;
#endif
}

bool PrepareCanonicalSortOrderTerm(
    const api::EngineRequestContext& context,
    const plan::CanonicalLogicalPropertyOrderingTerm& logical_term,
    const exec::ExecutorColumnDescriptor& column,
    const std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  if (term == nullptr || detail == nullptr) return false;
  *term = {};
  detail->clear();
  term->column = column_ordinal;
  term->expression_descriptor_id = column.descriptor_id;
  term->direction =
      logical_term.direction ==
              plan::CanonicalLogicalPropertySortDirection::kAscending
          ? exec::CanonicalDescriptorOrderDirection::ascending
          : exec::CanonicalDescriptorOrderDirection::descending;
  term->null_placement =
      logical_term.null_placement ==
              plan::CanonicalLogicalPropertyNullPlacement::kNullsFirst
          ? exec::CanonicalDescriptorNullPlacement::first
          : exec::CanonicalDescriptorNullPlacement::last;
  term->collation_uuid = logical_term.collation_uuid;

  if (column.descriptor.canonical_type_name == "text") {
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
    *term = {};
    *detail =
        "sort character ordering requires the production engine resource "
        "catalog";
    return false;
#else
    api::EngineUuid collation_uuid;
    collation_uuid.canonical = term->collation_uuid;
    const auto resolved = api::LookupEngineResourceDescriptorByUuid(
        context, collation_uuid, "collation");
    if (!resolved.ok || !resolved.resource_descriptor.present ||
        resolved.resource_descriptor.resource_uuid.canonical !=
            term->collation_uuid) {
      *term = {};
      *detail =
          "sort character ordering lacks current engine collation authority: " +
          (resolved.diagnostic.code.empty()
               ? std::string("CATALOG.RESOURCE.DESCRIPTOR_INVALID")
               : resolved.diagnostic.code);
      return false;
    }
    term->resource_epoch = resolved.resource_descriptor.resource_epoch;
    term->collation_epoch = resolved.resource_descriptor.family_epoch;
    term->text_seed.active = true;
    term->text_seed.seed_pack_name =
        resolved.resource_descriptor.seed_pack_name;
    term->text_seed.seed_pack_version =
        resolved.resource_descriptor.seed_pack_version;
    term->text_seed.charset_name =
        resolved.resource_descriptor.parent_canonical_name;
    term->text_seed.collation_name =
        resolved.resource_descriptor.canonical_name;
    term->text_seed.collation_case_insensitive =
        resolved.resource_descriptor.case_insensitive;
    term->text_seed.collation_accent_insensitive =
        resolved.resource_descriptor.accent_insensitive;
#endif
  } else if ((column.descriptor.canonical_type_name == "time" ||
              column.descriptor.canonical_type_name == "timestamp") &&
             column.descriptor.encoded_descriptor.find(
                 "timezone_profile_id=") != std::string::npos) {
    if (!BindTimezoneOrderAuthority(context, term, detail)) {
      *term = {};
      return false;
    }
  }
  const auto validation =
      exec::ValidateCanonicalDescriptorOrderTerm(*term, column);
  if (!validation.ok) {
    *term = {};
    *detail = validation.detail;
    return false;
  }
  return true;
}

PreparedSortRoot PrepareSortRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedSortRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail = "sort output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "sort root output lineage is not admitted by this profile";
    return result;
  }
  if (properties.properties.size() != 1 ||
      root.required_property_uuids.size() != 1 ||
      root.delivered_property_uuids.size() != 1 ||
      root.required_property_uuids.front() !=
          root.delivered_property_uuids.front()) {
    result.detail =
        "sort requires exactly one enforced ordering property";
    return result;
  }
  const auto& property = properties.properties.front();
  if (property.property_uuid != root.required_property_uuids.front() ||
      property.property_kind !=
          plan::CanonicalLogicalPropertyKind::kOrdering ||
      property.origin_logical_node_id != root.logical_node_id ||
      property.ordering_terms.empty()) {
    result.detail = "sort ordering property identity or origin is unresolved";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < input_node.output_descriptor_ids.size(); ++ordinal) {
    input_ordinals.emplace(input_node.output_descriptor_ids[ordinal], ordinal);
  }
  std::unordered_set<std::uint32_t> bound_order_expressions(
      root.bound_expression_ids.begin(), root.bound_expression_ids.end());
  if (bound_order_expressions.size() != property.ordering_terms.size()) {
    result.detail =
        "sort bound-expression coverage differs from its ordering terms";
    return result;
  }

  for (const auto& logical_term : property.ordering_terms) {
    const auto expression = expressions.find(logical_term.expression_id);
    if (expression == expressions.end() ||
        !bound_order_expressions.contains(logical_term.expression_id) ||
        std::ranges::find(input_node.bound_expression_ids,
                          logical_term.expression_id) ==
            input_node.bound_expression_ids.end()) {
      result.order_terms.clear();
      result.detail = "sort ordering expression is not bound to its input";
      return result;
    }
    const auto ordinal =
        input_ordinals.find(expression->second->result_descriptor_id);
    if (ordinal == input_ordinals.end()) {
      result.order_terms.clear();
      result.detail =
          "sort ordering expression does not resolve to an input descriptor";
      return result;
    }

    const auto& column = input.batch.columns[ordinal->second];
    exec::CanonicalDescriptorOrderTerm term;
    if (!PrepareCanonicalSortOrderTerm(
            context, logical_term, column, ordinal->second, &term,
            &result.detail)) {
      result.order_terms.clear();
      return result;
    }
    result.order_terms.push_back(std::move(term));
  }

  result.result_bindings = input.result_bindings;
  result.ordering_property_uuid = property.property_uuid;
  result.ok = true;
  return result;
}

PreparedDistinctRoot PrepareQueryDistinctRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& distinct_node,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedDistinctRoot result;
  if (distinct_node.output_descriptor_ids.empty() ||
      distinct_node.output_descriptor_ids != input_node.output_descriptor_ids ||
      distinct_node.bound_expression_ids.size() !=
          distinct_node.output_descriptor_ids.size() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail =
        "query DISTINCT does not cover and preserve its projected schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == distinct_node.logical_node_id;
      })) {
    result.detail =
        "query DISTINCT output lineage is not admitted by this profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  std::unordered_set<std::uint32_t> covered_descriptors;
  for (const auto expression_id : distinct_node.bound_expression_ids) {
    const auto expression = expressions.find(expression_id);
    if (expression == expressions.end() ||
        std::ranges::find(input_node.bound_expression_ids, expression_id) ==
            input_node.bound_expression_ids.end() ||
        std::ranges::find(distinct_node.output_descriptor_ids,
                          expression->second->result_descriptor_id) ==
            distinct_node.output_descriptor_ids.end() ||
        !covered_descriptors
             .insert(expression->second->result_descriptor_id)
             .second) {
      result.detail =
          "query DISTINCT expression coverage is unresolved or duplicated";
      return result;
    }
  }

  for (std::size_t column = 0; column < input.batch.columns.size(); ++column) {
    const auto descriptor_id = distinct_node.output_descriptor_ids[column];
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end() ||
        !covered_descriptors.contains(descriptor_id)) {
      result.equality_terms.clear();
      result.detail = "query DISTINCT descriptor coverage is incomplete";
      return result;
    }
    exec::CanonicalDescriptorOrderTerm term;
    term.column = column;
    term.expression_descriptor_id = descriptor_id;
    term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
    if (input.batch.columns[column].descriptor.canonical_type_name == "text") {
      if (!descriptor->second->collation_uuid.has_value()) {
        result.equality_terms.clear();
        result.detail =
            "query DISTINCT character equality lacks a bound collation";
        return result;
      }
      term.collation_uuid = *descriptor->second->collation_uuid;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
      result.equality_terms.clear();
      result.detail =
          "query DISTINCT character equality requires the production engine "
          "resource catalog";
      return result;
#else
      api::EngineUuid collation_uuid;
      collation_uuid.canonical = term.collation_uuid;
      const auto resolved = api::LookupEngineResourceDescriptorByUuid(
          context, collation_uuid, "collation");
      if (!resolved.ok || !resolved.resource_descriptor.present ||
          resolved.resource_descriptor.resource_uuid.canonical !=
              term.collation_uuid) {
        result.equality_terms.clear();
        result.detail =
            "query DISTINCT character equality lacks current engine "
            "collation authority";
        return result;
      }
      term.resource_epoch = resolved.resource_descriptor.resource_epoch;
      term.collation_epoch = resolved.resource_descriptor.family_epoch;
      term.text_seed.active = true;
      term.text_seed.seed_pack_name =
          resolved.resource_descriptor.seed_pack_name;
      term.text_seed.seed_pack_version =
          resolved.resource_descriptor.seed_pack_version;
      term.text_seed.charset_name =
          resolved.resource_descriptor.parent_canonical_name;
      term.text_seed.collation_name =
          resolved.resource_descriptor.canonical_name;
      term.text_seed.collation_case_insensitive =
          resolved.resource_descriptor.case_insensitive;
      term.text_seed.collation_accent_insensitive =
          resolved.resource_descriptor.accent_insensitive;
#endif
    } else if ((input.batch.columns[column].descriptor.canonical_type_name ==
                    "time" ||
                input.batch.columns[column].descriptor.canonical_type_name ==
                    "timestamp") &&
               input.batch.columns[column].descriptor.encoded_descriptor.find(
                   "timezone_profile_id=") != std::string::npos) {
      if (!BindTimezoneOrderAuthority(context, &term, &result.detail)) {
        result.equality_terms.clear();
        return result;
      }
    }
    const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
        term, input.batch.columns[column]);
    if (!validation.ok) {
      result.equality_terms.clear();
      result.detail = validation.detail;
      return result;
    }
    result.equality_terms.push_back(std::move(term));
  }

  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedLimitRoot PrepareLimitRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedLimitRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size()) {
    result.detail = "limit output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "limit root output lineage is not admitted by this profile";
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedProjectRoot PrepareDescriptorDirectProjectRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedProjectRoot result;
  if (root.output_descriptor_ids.empty() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail = "project input or output descriptor coverage is incomplete";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "descriptor-direct project does not admit root output lineage";
    return result;
  }

  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0;
       ordinal < input_node.output_descriptor_ids.size(); ++ordinal) {
    input_ordinals.emplace(input_node.output_descriptor_ids[ordinal], ordinal);
  }
  std::size_t published_ordinal = 0;
  for (std::size_t output_ordinal = 0;
       output_ordinal < root.output_descriptor_ids.size(); ++output_ordinal) {
    const auto source =
        input_ordinals.find(root.output_descriptor_ids[output_ordinal]);
    if (source == input_ordinals.end()) {
      result.projected_columns.clear();
      result.result_bindings.clear();
      result.detail =
          "descriptor-direct project output is not an input column";
      return result;
    }
    result.projected_columns.push_back(source->second);
    auto binding = input.result_bindings[source->second];
    binding.physical_column_ordinal = output_ordinal;
    if (binding.visible) {
      if (!binding.published_descriptor.has_value()) {
        result.projected_columns.clear();
        result.result_bindings.clear();
        result.detail = "project visible result binding is incomplete";
        return result;
      }
      binding.published_descriptor->ordinal =
          static_cast<std::uint32_t>(published_ordinal++);
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

bool PrepareInputRowBinding(
    const api::TypedRelationalDag& dag,
    const std::uint32_t root_expression_id,
    const std::vector<std::uint32_t>& input_descriptor_ids,
    CanonicalRelationalExpressionRowBinding* row_binding,
    std::string* detail) {
  if (row_binding == nullptr || detail == nullptr ||
      root_expression_id == 0 || input_descriptor_ids.empty()) {
    if (detail != nullptr) {
      *detail = "row expression binding request is incomplete";
    }
    return false;
  }
  *row_binding = {};
  row_binding->row_descriptor_ids = input_descriptor_ids;

  std::unordered_map<std::uint32_t, std::size_t> input_ordinals;
  for (std::size_t ordinal = 0; ordinal < input_descriptor_ids.size();
       ++ordinal) {
    if (!input_ordinals.emplace(input_descriptor_ids[ordinal], ordinal)
             .second) {
      *row_binding = {};
      *detail = "row expression input descriptor identity is ambiguous";
      return false;
    }
  }
  std::unordered_map<std::uint32_t,
                     const api::RelationalExpressionRecord*>
      expressions;
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  std::unordered_set<std::uint32_t> reachable;
  std::vector<std::uint32_t> pending{root_expression_id};
  while (!pending.empty()) {
    const auto expression_id = pending.back();
    pending.pop_back();
    if (!reachable.insert(expression_id).second) continue;
    const auto expression = expressions.find(expression_id);
    if (expression == expressions.end()) {
      *row_binding = {};
      *detail = "row expression has a dangling expression child";
      return false;
    }
    if (expression->second->expression_kind ==
        api::RelationalExpressionKind::kIdentifier) {
      const auto ordinal =
          input_ordinals.find(expression->second->result_descriptor_id);
      if (ordinal == input_ordinals.end()) {
        *row_binding = {};
        *detail = "row expression identifier is not supplied by its input";
        return false;
      }
      row_binding->slots.push_back(
          {expression_id, expression->second->result_descriptor_id,
           ordinal->second,
           CanonicalRelationalExpressionRowSlotKind::input_identifier});
    }
    pending.insert(pending.end(),
                   expression->second->child_expression_ids.begin(),
                   expression->second->child_expression_ids.end());
  }
  return true;
}

bool MaterializeExpressionSortBatch(
    const api::TypedRelationalDag& dag,
    const std::vector<PreparedSortExpression>& expressions,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* sort_batch,
    std::string* detail) {
  if (sort_batch == nullptr || detail == nullptr || expressions.empty() ||
      input_batch.columns.empty() || input_batch.rows.empty()) {
    if (detail != nullptr) {
      *detail = "expression SORT materialization request is incomplete";
    }
    return false;
  }
  *sort_batch = input_batch;
  detail->clear();
  for (const auto& expression : expressions) {
    sort_batch->columns.push_back(expression.materialized_column);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  for (std::size_t row_ordinal = 0;
       row_ordinal < input_batch.rows.size(); ++row_ordinal) {
    auto& sort_row = sort_batch->rows[row_ordinal];
    const auto& input_row = input_batch.rows[row_ordinal];
    sort_row.values.reserve(input_row.values.size() + expressions.size());
    for (const auto& expression : expressions) {
      api::EngineTypedValue value;
      if (!runtime.EvaluateForConsumer(
              expression.expression_id, expression.expected_type,
              expression.row_binding, input_row.values,
              api::EngineCanonicalExpressionConsumer::projection, &value,
              detail)) {
        *sort_batch = {};
        return false;
      }
      sort_row.values.push_back(std::move(value));
    }
  }

  std::vector<std::uint32_t> descriptor_ids;
  descriptor_ids.reserve(sort_batch->columns.size());
  for (const auto& column : sort_batch->columns) {
    descriptor_ids.push_back(column.descriptor_id);
  }
  const auto canonical =
      exec::ValidateCanonicalDescriptorBatch(*sort_batch, descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(*sort_batch);
  if (!canonical.ok || !values.ok) {
    *detail = !canonical.ok
                  ? canonical.diagnostic_code + ":" + canonical.detail
                  : values.diagnostic_code + ":" + values.detail;
    *sort_batch = {};
    return false;
  }
  return true;
}

PreparedSortRoot PrepareExpressionSortRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  PreparedSortRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size() ||
      input.batch.rows.empty()) {
    result.detail =
        "expression SORT output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "expression SORT root output lineage is not admitted by this profile";
    return result;
  }
  if (properties.properties.size() != 1 ||
      root.required_property_uuids.size() != 1 ||
      root.delivered_property_uuids.size() != 1 ||
      root.required_property_uuids.front() !=
          root.delivered_property_uuids.front()) {
    result.detail =
        "expression SORT requires exactly one enforced ordering property";
    return result;
  }
  const auto& property = properties.properties.front();
  if (property.property_uuid != root.required_property_uuids.front() ||
      property.property_kind !=
          plan::CanonicalLogicalPropertyKind::kOrdering ||
      property.origin_logical_node_id != root.logical_node_id ||
      property.ordering_terms.empty() ||
      root.bound_expression_ids.size() != property.ordering_terms.size()) {
    result.detail =
        "expression SORT ordering property or expression coverage is invalid";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expression_records;
  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_set<std::uint32_t> materialized_descriptor_ids(
      input_node.output_descriptor_ids.begin(),
      input_node.output_descriptor_ids.end());
  for (const auto& expression : dag.expressions) {
    expression_records.emplace(expression.expression_id, &expression);
  }
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  for (std::size_t ordinal = 0; ordinal < property.ordering_terms.size();
       ++ordinal) {
    const auto& logical_term = property.ordering_terms[ordinal];
    if (root.bound_expression_ids[ordinal] != logical_term.expression_id) {
      result.detail =
          "expression SORT bound-expression order differs from its property";
      return result;
    }
    const auto expression =
        expression_records.find(logical_term.expression_id);
    if (expression == expression_records.end()) {
      result.detail = "expression SORT term is unresolved";
      return result;
    }
    const auto descriptor =
        descriptors.find(expression->second->result_descriptor_id);
    if (descriptor == descriptors.end() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        !materialized_descriptor_ids
             .insert(expression->second->result_descriptor_id)
             .second) {
      result.detail =
          "expression SORT result descriptor is absent, ambiguous, or "
          "unresolved";
      return result;
    }

    PreparedSortExpression prepared_expression;
    prepared_expression.expression_id = logical_term.expression_id;
    if (!PrepareInputRowBinding(
            dag, prepared_expression.expression_id,
            input_node.output_descriptor_ids,
            &prepared_expression.row_binding, &result.detail) ||
        !runtime.InferTypeForConsumer(
            prepared_expression.expression_id,
            prepared_expression.row_binding, input.batch.rows.front().values,
            api::EngineCanonicalExpressionConsumer::projection,
            &prepared_expression.expected_type, &result.detail) ||
        prepared_expression.expected_type.empty() ||
        prepared_expression.expected_type == "null") {
      if (result.detail.empty()) {
        result.detail = "expression SORT result type is unresolved";
      }
      return result;
    }
    const auto type_id = dt::CanonicalTypeIdFromStableName(
        prepared_expression.expected_type);
    if (type_id == dt::CanonicalTypeId::unknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_id != dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         prepared_expression.expected_type != "timestamp")) {
      result.detail =
          "expression SORT descriptor metadata contradicts its result type";
      return result;
    }
    api::EngineTypedValue first_value;
    if (!runtime.EvaluateForConsumer(
            prepared_expression.expression_id,
            prepared_expression.expected_type,
            prepared_expression.row_binding, input.batch.rows.front().values,
            api::EngineCanonicalExpressionConsumer::projection, &first_value,
            &result.detail) ||
        first_value.descriptor.canonical_type_name !=
            prepared_expression.expected_type) {
      if (result.detail.empty()) {
        result.detail = "expression SORT first value has an invalid type";
      }
      return result;
    }
    prepared_expression.materialized_column = {
        "__sort_expression_" +
            std::to_string(prepared_expression.expression_id),
        first_value.descriptor,
        descriptor->second->nullability ==
            api::RelationalNullability::kNullable,
        descriptor->second->descriptor_id};
    exec::CanonicalDescriptorOrderTerm term;
    if (!PrepareCanonicalSortOrderTerm(
            context, logical_term,
            prepared_expression.materialized_column,
            input.batch.columns.size() + result.expressions.size(), &term,
            &result.detail)) {
      return result;
    }
    result.order_terms.push_back(std::move(term));
    result.expressions.push_back(std::move(prepared_expression));
  }

  if (!MaterializeExpressionSortBatch(
          dag, result.expressions, input.batch, expression_services,
          &result.expression_input_batch, &result.detail)) {
    result.order_terms.clear();
    result.expressions.clear();
    return result;
  }
  result.expression_ordering = true;
  result.result_bindings = input.result_bindings;
  result.ordering_property_uuid = property.property_uuid;
  result.ok = true;
  return result;
}

bool MaterializeExpressionProjectBatch(
    const api::TypedRelationalDag& dag,
    const std::vector<PreparedProjectExpression>& expressions,
    const std::vector<exec::ExecutorColumnDescriptor>& output_columns,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* output_batch,
    std::string* detail) {
  if (output_batch == nullptr || detail == nullptr || expressions.empty() ||
      expressions.size() != output_columns.size()) {
    if (detail != nullptr) {
      *detail = "expression PROJECT materialization request is incomplete";
    }
    return false;
  }
  *output_batch = {};
  detail->clear();
  output_batch->columns = output_columns;
  output_batch->rows.reserve(input_batch.rows.size());
  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  for (const auto& input_row : input_batch.rows) {
    exec::DescriptorTuple output_row;
    output_row.values.reserve(expressions.size());
    for (const auto& expression : expressions) {
      api::EngineTypedValue value;
      if (!runtime.EvaluateForConsumer(
              expression.expression_id, expression.expected_type,
              expression.row_binding, input_row.values,
              api::EngineCanonicalExpressionConsumer::projection, &value,
              detail)) {
        *output_batch = {};
        return false;
      }
      output_row.values.push_back(std::move(value));
    }
    output_batch->rows.push_back(std::move(output_row));
  }
  std::vector<std::uint32_t> descriptor_ids;
  descriptor_ids.reserve(output_columns.size());
  for (const auto& column : output_columns) {
    descriptor_ids.push_back(column.descriptor_id);
  }
  const auto canonical =
      exec::ValidateCanonicalDescriptorBatch(*output_batch, descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(*output_batch);
  if (!canonical.ok || !values.ok) {
    *detail = !canonical.ok
                  ? canonical.diagnostic_code + ":" + canonical.detail
                  : values.diagnostic_code + ":" + values.detail;
    *output_batch = {};
    return false;
  }
  return true;
}

PreparedProjectRoot PrepareExpressionProjectRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  PreparedProjectRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.bound_expression_ids.size() != root.output_descriptor_ids.size() ||
      input.result_bindings.size() != input.batch.columns.size() ||
      input_node.output_descriptor_ids.size() != input.batch.columns.size()) {
    result.detail =
        "expression PROJECT input, output, or expression coverage is incomplete";
    return result;
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == root.logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != root.output_descriptor_ids.size()) {
    result.detail = "expression PROJECT root output lineage is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t,
                     const api::RelationalExpressionRecord*>
      expression_records;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expression_records.emplace(expression.expression_id, &expression);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  std::vector<api::EngineTypedValue> descriptor_only_input_row;
  if (input.batch.rows.empty()) {
    descriptor_only_input_row.reserve(input.batch.columns.size());
    for (const auto& column : input.batch.columns) {
      api::EngineTypedValue value;
      value.descriptor = column.descriptor;
      value.state = api::EngineValueState::value;
      value.is_null = false;
      descriptor_only_input_row.push_back(std::move(value));
    }
  }
  const auto& type_inference_row =
      input.batch.rows.empty() ? descriptor_only_input_row
                               : input.batch.rows.front().values;
  std::vector<exec::ExecutorColumnDescriptor> output_columns;
  output_columns.reserve(outputs.size());
  std::size_t published_ordinal = 0;
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto expression_id = root.bound_expression_ids[ordinal];
    const auto expression = expression_records.find(expression_id);
    const auto descriptor = descriptors.find(root.output_descriptor_ids[ordinal]);
    const auto* output = outputs[ordinal];
    if (expression == expression_records.end() ||
        descriptor == descriptors.end() || output->ordinal != ordinal ||
        output->expression_id != expression_id ||
        output->descriptor_id != root.output_descriptor_ids[ordinal] ||
        expression->second->result_descriptor_id !=
            root.output_descriptor_ids[ordinal] ||
        output->output_name_utf8.empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown) {
      result.detail =
          "expression PROJECT output expression or descriptor binding is invalid";
      return result;
    }

    PreparedProjectExpression prepared_expression;
    prepared_expression.expression_id = expression_id;
    if (!PrepareInputRowBinding(
            dag, expression_id, input_node.output_descriptor_ids,
            &prepared_expression.row_binding, &result.detail) ||
        !runtime.InferTypeForConsumer(
            expression_id, prepared_expression.row_binding,
            type_inference_row,
            api::EngineCanonicalExpressionConsumer::projection,
            &prepared_expression.expected_type, &result.detail) ||
        prepared_expression.expected_type.empty() ||
        prepared_expression.expected_type == "null") {
      if (result.detail.empty()) {
        result.detail = "expression PROJECT result type is unresolved";
      }
      return result;
    }
    const auto type_id = dt::CanonicalTypeIdFromStableName(
        prepared_expression.expected_type);
    if (type_id == dt::CanonicalTypeId::unknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_id != dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         prepared_expression.expected_type != "timestamp")) {
      result.detail =
          "expression PROJECT descriptor metadata contradicts its result type";
      return result;
    }
    api::EngineDescriptor output_descriptor;
    if (!input.batch.rows.empty()) {
      api::EngineTypedValue first_value;
      if (!runtime.EvaluateForConsumer(
              expression_id, prepared_expression.expected_type,
              prepared_expression.row_binding,
              input.batch.rows.front().values,
              api::EngineCanonicalExpressionConsumer::projection,
              &first_value, &result.detail)) {
        return result;
      }
      output_descriptor = std::move(first_value.descriptor);
    } else {
      const auto& source = *descriptor->second;
      output_descriptor.descriptor_uuid.canonical = source.descriptor_uuid;
      output_descriptor.descriptor_kind = "scalar";
      output_descriptor.canonical_type_name =
          prepared_expression.expected_type;
      output_descriptor.encoded_descriptor =
          "type_uuid=" + source.type_uuid + ";nullability=" +
          (source.nullability == api::RelationalNullability::kNullable
               ? "nullable"
               : "non_null");
      if (source.collation_uuid.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";collation_uuid=" + *source.collation_uuid;
      }
      if (source.timezone_profile_id.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";timezone_profile_id=" + *source.timezone_profile_id;
      }
      if (source.width.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";width=" + std::to_string(*source.width);
      }
      if (source.precision.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";precision=" + std::to_string(*source.precision);
      }
      if (source.scale.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";scale=" + std::to_string(*source.scale);
      }
    }
    output_columns.push_back(
        {output->output_name_utf8, std::move(output_descriptor),
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});
    result.expressions.push_back(std::move(prepared_expression));

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = output->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          output->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  if (!MaterializeExpressionProjectBatch(
          dag, result.expressions, output_columns, input.batch,
          expression_services, &result.expression_output_batch,
          &result.detail)) {
    result.expressions.clear();
    result.result_bindings.clear();
    return result;
  }
  result.expression_projection = true;
  result.ok = true;
  return result;
}

PreparedFilterRoot PrepareFilterRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input) {
  PreparedFilterRoot result;
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != input_node.output_descriptor_ids ||
      input.result_bindings.size() != input.batch.columns.size()) {
    result.detail = "filter output does not preserve its bound input schema";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "filter root output lineage is not admitted by this profile";
    return result;
  }
  if (root.bound_expression_ids.size() != 1) {
    result.detail = "filter predicate root identity is not exact";
    return result;
  }
  result.predicate_expression_id = root.bound_expression_ids.front();
  if (!PrepareInputRowBinding(
          dag, result.predicate_expression_id,
          input_node.output_descriptor_ids, &result.predicate_row_binding,
          &result.detail)) {
    return result;
  }
  result.result_bindings = input.result_bindings;
  result.ok = true;
  return result;
}

PreparedJoinRoot PrepareJoinRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    const MaterializedValues& left,
    const MaterializedValues& right,
    const exec::CanonicalAcceptedJoinKind join_kind) {
  PreparedJoinRoot result;
  std::vector<std::uint32_t> predicate_descriptors =
      left_node.output_descriptor_ids;
  predicate_descriptors.insert(predicate_descriptors.end(),
                               right_node.output_descriptor_ids.begin(),
                               right_node.output_descriptor_ids.end());
  std::vector<std::uint32_t> expected_descriptors =
      left_node.output_descriptor_ids;
  const bool left_only =
      join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi ||
      join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti;
  if (!left_only) {
    expected_descriptors.insert(expected_descriptors.end(),
                                right_node.output_descriptor_ids.begin(),
                                right_node.output_descriptor_ids.end());
  }
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != expected_descriptors ||
      left.result_bindings.size() != left.batch.columns.size() ||
      right.result_bindings.size() != right.batch.columns.size()) {
    result.detail = "join output does not preserve its accepted bound shape";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "join root output lineage is not admitted by this profile";
    return result;
  }
  const auto columns_are_nullable = [](const exec::DescriptorBatch& batch) {
    return std::ranges::all_of(batch.columns, [](const auto& column) {
      return column.nullable &&
             column.descriptor.encoded_descriptor.find(
                 "nullability=nullable") != std::string::npos;
    });
  };
  if ((join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
       join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) &&
      !columns_are_nullable(left.batch)) {
    result.detail =
        "outer join left NULL extension lacks nullable descriptor authority";
    return result;
  }
  if ((join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
       join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) &&
      !columns_are_nullable(right.batch)) {
    result.detail =
        "outer join right NULL extension lacks nullable descriptor authority";
    return result;
  }

  if (join_kind != exec::CanonicalAcceptedJoinKind::kCross) {
    if (root.bound_expression_ids.size() != 1) {
      result.detail = "join predicate root identity is not exact";
      return result;
    }
    result.predicate_expression_id = root.bound_expression_ids.front();
    if (!PrepareInputRowBinding(
            dag, result.predicate_expression_id, predicate_descriptors,
            &result.predicate_row_binding, &result.detail)) {
      if (result.detail ==
          "row expression input descriptor identity is ambiguous") {
        result.detail = "join predicate input descriptor identity is ambiguous";
      } else if (result.detail ==
                 "row expression identifier is not supplied by its input") {
        result.detail =
            "join predicate identifier is not supplied by either input";
      } else if (result.detail ==
                 "row expression has a dangling expression child") {
        result.detail = "join predicate has a dangling expression child";
      }
      return result;
    }
  }

  std::size_t published_ordinal = 0;
  const auto append_bindings =
      [&](const std::vector<exec::CanonicalResultColumnBinding>& bindings,
          const std::size_t physical_base) {
    for (const auto& source : bindings) {
      auto binding = source;
      binding.physical_column_ordinal += physical_base;
      if (binding.visible) {
        if (!binding.published_descriptor.has_value()) return false;
        binding.published_descriptor->ordinal =
            static_cast<std::uint32_t>(published_ordinal++);
      }
      result.result_bindings.push_back(std::move(binding));
    }
    return true;
  };
  if (!append_bindings(left.result_bindings, 0) ||
      (!left_only &&
       !append_bindings(right.result_bindings, left.batch.columns.size()))) {
    result.result_bindings.clear();
    result.detail = "join visible result binding is incomplete";
    return result;
  }
  result.ok = true;
  return result;
}

LiveSetOperationProfile MatchLiveSetOperationProfile(
    const std::string_view semantic_variant_id) {
  LiveSetOperationProfile profile;
  constexpr std::string_view kPrefix = "set-operation.";
  constexpr std::string_view kSuffix = ".v1";
  if (!semantic_variant_id.starts_with(kPrefix) ||
      !semantic_variant_id.ends_with(kSuffix) ||
      semantic_variant_id.size() <= kPrefix.size() + kSuffix.size()) {
    return profile;
  }
  const auto payload = semantic_variant_id.substr(
      kPrefix.size(),
      semantic_variant_id.size() - kPrefix.size() - kSuffix.size());
  const auto first_modifier = payload.find('.');
  const auto base = payload.substr(0, first_modifier);
  std::string operation_component;
  std::string quantifier_component;
  if (base == "union-all") {
    profile.operation = exec::CanonicalSetOperationKind::kUnion;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "union";
    quantifier_component = "all";
    profile.operation_name = "UNION ALL";
  } else if (base == "union-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kUnion;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "union";
    quantifier_component = "distinct";
    profile.operation_name = "UNION DISTINCT";
  } else if (base == "intersect-all") {
    profile.operation = exec::CanonicalSetOperationKind::kIntersect;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "intersect";
    quantifier_component = "all";
    profile.operation_name = "INTERSECT ALL";
  } else if (base == "intersect-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kIntersect;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "intersect";
    quantifier_component = "distinct";
    profile.operation_name = "INTERSECT DISTINCT";
  } else if (base == "except-all") {
    profile.operation = exec::CanonicalSetOperationKind::kExcept;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "except";
    quantifier_component = "all";
    profile.operation_name = "EXCEPT ALL";
  } else if (base == "except-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kExcept;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "except";
    quantifier_component = "distinct";
    profile.operation_name = "EXCEPT DISTINCT";
  } else {
    return profile;
  }

  bool by_name = false;
  bool type_reconciled = false;
  bool null_collation = false;
  std::size_t modifier_begin = first_modifier;
  while (modifier_begin != std::string_view::npos) {
    ++modifier_begin;
    const auto modifier_end = payload.find('.', modifier_begin);
    const auto modifier = payload.substr(
        modifier_begin,
        modifier_end == std::string_view::npos
            ? std::string_view::npos
            : modifier_end - modifier_begin);
    if (modifier == "by-name" && !by_name && !type_reconciled &&
        !null_collation) {
      by_name = true;
    } else if (modifier == "type-reconciled" && !type_reconciled &&
               !null_collation) {
      type_reconciled = true;
    } else if (modifier == "null-collation" && !null_collation) {
      null_collation = true;
    } else {
      return {};
    }
    modifier_begin = modifier_end;
  }

  profile.alignment =
      by_name ? exec::CanonicalSetOperationAlignment::kByName
              : exec::CanonicalSetOperationAlignment::kOrdinal;
  profile.type_profile =
      type_reconciled
          ? exec::CanonicalSetOperationTypeProfile::kLosslessImplicit
          : exec::CanonicalSetOperationTypeProfile::kExact;
  profile.equality_profile =
      null_collation
          ? exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation
          : exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  profile.implementation_id = "setop." + operation_component + "-" +
                              quantifier_component + "." +
                              (by_name ? "by-name" : "ordinal");
  if (type_reconciled) {
    profile.implementation_id += ".type-reconciled";
  }
  if (null_collation) {
    profile.implementation_id += ".null-collation";
  }
  profile.implementation_id += ".typed.v1";
  profile.identity_component = operation_component + "-" +
                               quantifier_component +
                               (by_name ? ".by-name" : ".ordinal") +
                               (type_reconciled ? ".type-reconciled" : "") +
                               (null_collation ? ".null-collation" : "");
  profile.physical_semantic_id =
      "canonical.setop." + profile.identity_component + ".v1";
  if (by_name) profile.operation_name += " BY NAME";
  profile.matched = true;
  return profile;
}

PreparedSetOperationRoot PrepareSetOperationRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right,
    const LiveSetOperationProfile& profile) {
  PreparedSetOperationRoot result;
  if (!profile.matched || root.output_descriptor_ids.empty() ||
      left.batch.columns.size() != root.output_descriptor_ids.size() ||
      right.batch.columns.size() != root.output_descriptor_ids.size() ||
      left.result_bindings.size() != root.output_descriptor_ids.size() ||
      right.result_bindings.size() != root.output_descriptor_ids.size()) {
    result.detail = "set-operation input/result arity is inconsistent";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "set-operation root output lineage is not admitted by this profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }

  std::unordered_map<std::string, std::size_t> right_name_ordinals;
  if (profile.alignment == exec::CanonicalSetOperationAlignment::kByName) {
    std::unordered_set<std::string> left_names;
    for (const auto& column : left.batch.columns) {
      if (column.stable_name.empty() ||
          !left_names.insert(column.stable_name).second) {
        result.detail = "set-operation BY NAME left names are not unique";
        return result;
      }
    }
    for (std::size_t column = 0; column < right.batch.columns.size();
         ++column) {
      if (right.batch.columns[column].stable_name.empty() ||
          !right_name_ordinals
               .emplace(right.batch.columns[column].stable_name, column)
               .second) {
        result.detail = "set-operation BY NAME right names are not unique";
        return result;
      }
    }
    if (left_names.size() != right_name_ordinals.size()) {
      result.detail = "set-operation BY NAME column sets differ";
      return result;
    }
  }

  const auto descriptor_has_type_uuid = [](const api::EngineDescriptor& value,
                                           const std::string_view type_uuid) {
    const auto token = "type_uuid=" + std::string(type_uuid);
    return value.encoded_descriptor == token ||
           value.encoded_descriptor.starts_with(token + ";");
  };
  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < root.output_descriptor_ids.size();
       ++column) {
    const auto descriptor = descriptors.find(root.output_descriptor_ids[column]);
    const auto& left_column = left.batch.columns[column];
    std::size_t right_column_ordinal = column;
    if (profile.alignment == exec::CanonicalSetOperationAlignment::kByName) {
      const auto right_ordinal =
          right_name_ordinals.find(left_column.stable_name);
      if (right_ordinal == right_name_ordinals.end()) {
        result.detail = "set-operation BY NAME column sets differ";
        return result;
      }
      right_column_ordinal = right_ordinal->second;
    }
    const auto& right_column = right.batch.columns[right_column_ordinal];
    if (descriptor == descriptors.end() ||
        descriptor->second->nullability == api::RelationalNullability::kUnknown ||
        left_column.descriptor.canonical_type_name.empty() ||
        right_column.descriptor.canonical_type_name.empty()) {
      result.detail = "set-operation descriptor reconciliation is unresolved";
      return result;
    }

    std::string result_type_name;
    if (descriptor_has_type_uuid(left_column.descriptor,
                                 descriptor->second->type_uuid)) {
      result_type_name = left_column.descriptor.canonical_type_name;
    } else if (descriptor_has_type_uuid(right_column.descriptor,
                                        descriptor->second->type_uuid)) {
      result_type_name = right_column.descriptor.canonical_type_name;
    } else if (left_column.descriptor.canonical_type_name ==
               right_column.descriptor.canonical_type_name) {
      result_type_name = left_column.descriptor.canonical_type_name;
    } else {
      result.detail =
          "set-operation common result type is not bound to either input";
      return result;
    }

    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = result_type_name;
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    const bool nullable = descriptor->second->nullability ==
                          api::RelationalNullability::kNullable;
    if (nullable != (left_column.nullable || right_column.nullable)) {
      result.detail =
          "set-operation result nullability does not cover both inputs";
      return result;
    }
    if (profile.type_profile ==
            exec::CanonicalSetOperationTypeProfile::kExact &&
        (engine_descriptor.canonical_type_name !=
             left_column.descriptor.canonical_type_name ||
         engine_descriptor.canonical_type_name !=
             right_column.descriptor.canonical_type_name ||
         engine_descriptor.encoded_descriptor !=
             left_column.descriptor.encoded_descriptor ||
         engine_descriptor.encoded_descriptor !=
             right_column.descriptor.encoded_descriptor)) {
      result.detail =
          "set-operation exact descriptors do not share one bound type";
      return result;
    }

    if (result_type_name == "text" &&
        profile.equality_profile ==
            exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation) {
      if (!descriptor->second->collation_uuid.has_value()) {
        result.detail =
            "set-operation character equality lacks a bound collation";
        return result;
      }
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
      (void)context;
      result.detail =
          "set-operation character equality requires the production engine "
          "resource catalog";
      return result;
#else
      api::EngineUuid collation_uuid;
      collation_uuid.canonical = *descriptor->second->collation_uuid;
      const auto resolved = api::LookupEngineResourceDescriptorByUuid(
          context, collation_uuid, "collation");
      if (!resolved.ok || !resolved.resource_descriptor.present ||
          resolved.resource_descriptor.resource_uuid.canonical !=
              collation_uuid.canonical) {
        result.detail =
            "set-operation character equality lacks current engine "
            "collation authority";
        return result;
      }
      exec::CanonicalSetOperationCollationBinding binding;
      binding.result_column = column;
      binding.collation_uuid = collation_uuid.canonical;
      binding.resource_epoch =
          resolved.resource_descriptor.resource_epoch;
      binding.collation_epoch = resolved.resource_descriptor.family_epoch;
      binding.text_seed.active = true;
      binding.text_seed.seed_pack_name =
          resolved.resource_descriptor.seed_pack_name;
      binding.text_seed.seed_pack_version =
          resolved.resource_descriptor.seed_pack_version;
      binding.text_seed.charset_name =
          resolved.resource_descriptor.parent_canonical_name;
      binding.text_seed.collation_name =
          resolved.resource_descriptor.canonical_name;
      binding.text_seed.collation_case_insensitive =
          resolved.resource_descriptor.case_insensitive;
      binding.text_seed.collation_accent_insensitive =
          resolved.resource_descriptor.accent_insensitive;
      result.collation_bindings.push_back(std::move(binding));
#endif
    } else if (descriptor->second->collation_uuid.has_value() &&
               result_type_name != "text") {
      result.detail = "set-operation collation targets a non-character type";
      return result;
    }

    result.result_columns.push_back(
        {left_column.stable_name, engine_descriptor,
         nullable,
         descriptor->second->descriptor_id});
    auto binding = left.result_bindings[column];
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          left_column.stable_name,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          std::nullopt,
          std::nullopt};
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

bool AddBatchMemoryBytes(const exec::DescriptorBatch& batch,
                         std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.encoded_value.size();
    }
  }
  return true;
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 && right >
                        std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

bool EvaluateNonNegativeRowBound(
    CanonicalRelationalExpressionRuntime* runtime,
    const std::uint32_t expression_id,
    std::uint64_t* row_bound,
    std::string* refusal_detail) {
  if (runtime == nullptr || row_bound == nullptr || refusal_detail == nullptr) {
    return false;
  }
  api::EngineTypedValue value;
  if (!runtime->EvaluateForConsumer(
          expression_id, "int64",
          api::EngineCanonicalExpressionConsumer::projection, &value,
          refusal_detail)) {
    return false;
  }
  if (value.state != api::EngineValueState::value || value.is_null ||
      value.descriptor.canonical_type_name != "int64" ||
      value.encoded_value.empty()) {
    *refusal_detail = "row bound is not a non-NULL canonical int64 value";
    return false;
  }
  std::int64_t decoded = 0;
  const auto [end, error] = std::from_chars(
      value.encoded_value.data(),
      value.encoded_value.data() + value.encoded_value.size(), decoded);
  if (error != std::errc{} ||
      end != value.encoded_value.data() + value.encoded_value.size() ||
      decoded < 0) {
    *refusal_detail = "row bound is negative or outside exact int64 admission";
    return false;
  }
  *row_bound = static_cast<std::uint64_t>(decoded);
  return true;
}

MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node,
    const CanonicalRelationalExpressionRuntimeServices& expression_services) {
  MaterializedValues result;
  const auto node_it = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == logical_node.logical_node_id;
      });
  if (node_it == dag.nodes.end() ||
      node_it->node_kind != api::RelationalDagNodeKind::kValues ||
      !node_it->input_node_ids.empty() || node_it->values_row_ids.empty() ||
      node_it->output_descriptor_ids.empty()) {
    result.detail = "live VALUES root shape is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalValuesRowRecord*> rows;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& row : dag.values_rows) rows.emplace(row.row_id, &row);
  CanonicalRelationalExpressionRuntime expression_runtime(
      dag, expression_services);

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == node_it->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != node_it->output_descriptor_ids.size()) {
    result.detail = "live VALUES result output coverage is incomplete";
    return result;
  }

  std::vector<std::string> type_names(node_it->output_descriptor_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto row = rows.find(row_id);
    if (row == rows.end() ||
        row->second->expression_ids.size() != type_names.size()) {
      result.detail = "live VALUES row width is inconsistent";
      return result;
    }
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      const auto expression =
          expressions.find(row->second->expression_ids[column]);
      if (expression == expressions.end() ||
          expression->second->result_descriptor_id !=
              node_it->output_descriptor_ids[column]) {
        result.detail =
            "live VALUES expression result descriptor is not column-bound";
        return result;
      }
      std::string type_name;
      if (!expression_runtime.InferType(expression->first, std::nullopt,
                                        &type_name, &result.detail)) {
        return result;
      }
      if (type_name == "null") continue;
      if (!type_names[column].empty() &&
          dt::CanonicalTypeIdFromStableName(type_names[column]) !=
              dt::CanonicalTypeIdFromStableName(type_name)) {
        result.detail = "live VALUES column has unreconciled literal types";
        return result;
      }
      type_names[column] = type_name;
    }
  }

  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      std::string reconciled_type;
      if (type_names[column].empty() ||
          !expression_runtime.InferType(row->expression_ids[column],
                                        type_names[column], &reconciled_type,
                                        &result.detail)) {
        if (result.detail.empty()) {
          result.detail = "live VALUES column type is unresolved";
        }
        return result;
      }
    }
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < type_names.size(); ++column) {
    const auto descriptor =
        descriptors.find(node_it->output_descriptor_ids[column]);
    if (descriptor == descriptors.end() || type_names[column].empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        (descriptor->second->collation_uuid.has_value() &&
         dt::CanonicalTypeIdFromStableName(type_names[column]) !=
             dt::CanonicalTypeId::character) ||
        (descriptor->second->timezone_profile_id.has_value() &&
         type_names[column] != "timestamp") ||
        outputs[column]->ordinal != column ||
        outputs[column]->descriptor_id !=
            node_it->output_descriptor_ids[column] ||
        outputs[column]->output_name_utf8.empty()) {
      result.detail = "live VALUES descriptor or output binding is unresolved";
      return result;
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = type_names[column];
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    result.batch.columns.push_back(
        {outputs[column]->output_name_utf8, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = column;
    binding.visible = outputs[column]->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          outputs[column]->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  result.batch.rows.reserve(node_it->values_row_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    exec::DescriptorTuple tuple;
    tuple.values.reserve(row->expression_ids.size());
    for (std::size_t column = 0; column < row->expression_ids.size(); ++column) {
      api::EngineTypedValue value;
      if (!expression_runtime.EvaluateForConsumer(
              row->expression_ids[column], type_names[column],
              api::EngineCanonicalExpressionConsumer::projection, &value,
              &result.detail)) {
        result.batch = {};
        result.result_bindings.clear();
        return result;
      }
      tuple.values.push_back(std::move(value));
    }
    result.batch.rows.push_back(std::move(tuple));
  }
  const auto canonical = exec::ValidateCanonicalDescriptorBatch(
      result.batch, node_it->output_descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(result.batch);
  if (!canonical.ok || !values.ok) {
    result.batch = {};
    result.result_bindings.clear();
    result.detail = !canonical.ok ? canonical.diagnostic_code + ":" + canonical.detail
                                  : values.diagnostic_code + ":" + values.detail;
    return result;
  }
  result.ok = true;
  return result;
}

api::EngineApiResult SuccessfulApiResult(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const api::CanonicalOptimizerSelectedExecutionResult& execution) {
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "query.execute";
  result.result_shape.result_kind = "rows";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  for (const auto& column : execution.result_publication.row_stream.columns) {
    result.result_shape.columns.push_back(column.descriptor);
  }
  for (const auto& row : execution.result_publication.row_stream.rows) {
    api::EngineRowValue api_row;
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      api_row.fields.emplace_back(
          execution.result_publication.envelope.column_descriptors[column]
              .name_utf8,
          row.values[column]);
    }
    result.result_shape.rows.push_back(std::move(api_row));
  }
  result.evidence.push_back(
      {"canonical.selected_plan",
       execution.dispatch.selected_plan_uuid});
  result.evidence.push_back(
      {"canonical.result_abi", "QOW-RESULT-DIAGNOSTIC-ABI-V1"});
  return result;
}

struct LivePhysicalNodeProfile {
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  plan::CanonicalLogicalRelationalNodeKind logical_node_kind{
      plan::CanonicalLogicalRelationalNodeKind::kValues};
  exec::PhysicalNodeKind physical_node_kind{exec::PhysicalNodeKind::kValues};
  std::string transformation_rule_id;
  std::uint64_t estimated_rows{0};
  std::uint64_t memory_bytes_required{0};
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<plan::CanonicalLogicalPropertyKind> supported_property_kinds;
  std::uint64_t page_read_sequential_units{0};
  std::uint64_t mga_visibility_checks_expected{0};
  bool storage_read_capable{false};
  bool mga_visibility_capable{false};
};

struct LivePhysicalPlanningResult {
  bool ok{false};
  exec::TypedPhysicalNodeDag physical_dag;
  std::string diagnostic_id;
  std::string detail;
};

LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::string_view selected_plan_purpose,
    const std::string_view operation_name) {
  LivePhysicalPlanningResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  if (profiles.size() != graph.nodes.size()) {
    result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
    result.detail = std::string(operation_name) +
                    " live profile does not cover every logical node";
    return result;
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "relational.calibration");
  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;
  alternatives.mga_statement_context = graph.mga_statement_context;

  std::unordered_set<std::uint32_t> covered_nodes;
  std::unordered_set<std::string> published_capabilities;
  std::vector<opt::CanonicalOptimizerSearchCandidateInput> candidates;
  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  for (const auto& profile : profiles) {
    const auto node = std::ranges::find_if(graph.nodes, [&](const auto& item) {
      return item.logical_node_id == profile.logical_node_id;
    });
    if (profile.logical_node_id == 0 || node == graph.nodes.end() ||
        node->node_kind != profile.logical_node_kind ||
        profile.implementation_id.empty() || profile.capability_uuid.empty() ||
        profile.transformation_rule_id.empty() ||
        profile.memory_bytes_required == 0 ||
        !covered_nodes.insert(profile.logical_node_id).second) {
      result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
      result.detail = std::string(operation_name) +
                      " live node profile is incomplete or inconsistent";
      return result;
    }
    const auto suffix = std::to_string(profile.logical_node_id);
    const auto alternative_uuid =
        DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
    alternatives.alternatives.push_back(
        {alternative_uuid,
         profile.logical_node_id,
         profile.implementation_id,
         profile.capability_uuid,
         node->output_descriptor_ids,
         true,
         {},
         profile.required_property_uuids,
         profile.delivered_property_uuids});

    opt::CanonicalOptimizerSearchCandidateInput candidate;
    candidate.alternative_uuid = alternative_uuid;
    candidate.transformation_uuid =
        DerivedCanonicalUuid(identity_scope, "transformation." + suffix);
    candidate.transformation_rule_id = profile.transformation_rule_id;
    candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    candidate.statistics_snapshot_uuid =
        request.optimizer_admission.statistics_snapshot_uuid;
    candidate.statistics_generation =
        request.optimizer_admission.statistics_generation;
    candidate.model_family_id = "relational.local.v1";
    candidate.cost_terms.cost_vector_uuid =
        DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
    candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
    candidate.cost_terms.cpu_units =
        std::max<std::uint64_t>(1, profile.estimated_rows);
    candidate.cost_terms.page_read_sequential_units =
        profile.page_read_sequential_units;
    candidate.cost_terms.memory_bytes_required =
        profile.memory_bytes_required;
    candidate.cost_terms.mga_visibility_checks_expected =
        profile.mga_visibility_checks_expected;
    candidate.cost_terms.confidence = opt::CostConfidence::kHigh;
    candidate.semantic_preserving = true;
    candidate.derived_from_admitted_statistics = true;
    candidate.engine_coster_owned = true;
    candidates.push_back(std::move(candidate));

    if (published_capabilities.insert(profile.capability_uuid).second) {
      opt::CanonicalExecutorCapabilityRecord capability;
      capability.capability_uuid = profile.capability_uuid;
      capability.capability_abi_version = 1;
      capability.implementation_id = profile.implementation_id;
      capability.logical_node_kind = profile.logical_node_kind;
      capability.physical_node_kind = profile.physical_node_kind;
      capability.minimum_input_count = profile.minimum_input_count;
      capability.maximum_input_count = profile.maximum_input_count;
      capability.maximum_memory_bytes =
          request.optimizer_request.resource.memory_budget_bytes;
      capability.supported_property_kinds =
          profile.supported_property_kinds;
      capability.spill_supported = false;
      capability.storage_read_capable = profile.storage_read_capable;
      capability.mga_visibility_capable = profile.mga_visibility_capable;
      capability.available = true;
      capability.engine_owned = true;
      capabilities.capabilities.push_back(std::move(capability));
    }
  }

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      candidates, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    result.diagnostic_id =
        search.issues.empty() ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                              : search.issues.front().diagnostic_id;
    result.detail = search.issues.empty()
                        ? std::string(operation_name) +
                              " live search returned no selected plan"
                        : search.issues.front().field_id;
    return result;
  }

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, selected_plan_purpose);
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    result.diagnostic_id =
        publication.issues.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : publication.issues.front().diagnostic_id;
    result.detail = publication.issues.empty()
                        ? std::string(operation_name) +
                              " live physical DAG was not published"
                        : publication.issues.front().field_id;
    return result;
  }
  result.ok = true;
  result.physical_dag = publication.physical_dag;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = std::string(kValuesImplementationId);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batches = std::move(batches), diagnostic_id = std::move(diagnostic_id),
       operation_name = std::move(operation_name)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        const auto batch = batches.find(node.relational_node_id);
        if (!inputs.empty() || batch == batches.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = diagnostic_id;
          step.diagnostic.detail = operation_name +
                                   " VALUES executor input or payload identity differs";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch->second.rows.size();
        step.rows_examined = batch->second.rows.size();
        step.materialized_output_batch = batch->second;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveProjectRegistration(
    const PreparedProjectRoot& prepared_root,
    std::string implementation_id,
    std::string capability_uuid,
    const std::size_t expected_input_row_count,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kProject;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [projected_columns = prepared_root.projected_columns,
       expression_projection = prepared_root.expression_projection,
       expressions = prepared_root.expressions,
       expression_output_columns =
           prepared_root.expression_output_batch.columns,
       relational_dag = std::move(relational_dag),
       expression_services = std::move(expression_services),
       expected_input_row_count, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() !=
                expected_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PROJECT-INPUT-V1";
          step.diagnostic.detail =
              "PROJECT input cardinality differs from its selected cost";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (expression_projection) {
          const auto input_validation = exec::ValidateCanonicalDescriptorBatch(
              input_batch, inputs.front().output_descriptor_ids);
          if (!input_validation.ok) {
            step.diagnostic = input_validation;
            return step;
          }
          const auto mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context, dag);
          const auto before =
              exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                              dag);
          if (!before.ok) {
            step.diagnostic = before;
            return step;
          }
          exec::DescriptorBatch output_batch;
          std::string expression_detail;
          if (!MaterializeExpressionProjectBatch(
                  relational_dag, expressions, expression_output_columns,
                  input_batch, expression_services, &output_batch,
                  &expression_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXPRESSION-V1";
            step.diagnostic.detail = std::move(expression_detail);
            return step;
          }
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  output_batch, node.output_descriptor_ids);
          if (!output_validation.ok) {
            step.diagnostic = output_validation;
            return step;
          }
          const auto after =
              exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                              dag);
          if (!after.ok) {
            step.diagnostic = after;
            return step;
          }
          step.result_handle_id = node.physical_node_id;
          step.input_row_count = input_batch.rows.size();
          step.rows_examined = input_batch.rows.size();
          step.output_row_count = output_batch.rows.size();
          step.materialized_output_batch = std::move(output_batch);
          step.mga_statement_context = mga_authority.statement_context;
          return step;
        }
        exec::CanonicalDescriptorProjectionRequest project_request;
        project_request.physical_dag = dag;
        project_request.selected_physical_node_id = node.physical_node_id;
        project_request.input_batch = input_batch;
        project_request.projected_columns = projected_columns;
        project_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto project_result =
            exec::ExecuteCanonicalDescriptorProjection(project_request);
        if (!project_result.diagnostic.ok) {
          step.diagnostic = project_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = project_result.output_batch.rows.size();
        step.materialized_output_batch = project_result.output_batch;
        step.mga_statement_context = project_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveFilterRegistration(
    std::vector<api::EngineSqlTruthValue> predicate_truth_values,
    std::string capability_uuid,
    const std::size_t expected_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kFilter;
  registration.implementation_id = "filter.3vl.row.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [predicate_truth_values = std::move(predicate_truth_values),
       expected_input_row_count, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != expected_input_row_count ||
            predicate_truth_values.size() != expected_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-FILTER-INPUT-V1";
          step.diagnostic.detail =
              "FILTER input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorFilterRequest filter_request;
        filter_request.physical_dag = dag;
        filter_request.selected_physical_node_id = node.physical_node_id;
        filter_request.input_batch = input_batch;
        filter_request.row_truth_values = predicate_truth_values;
        filter_request.consumer = api::EnginePredicateConsumer::filter;
        filter_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto filter_result =
            exec::ExecuteCanonicalDescriptorFilter(filter_request);
        if (!filter_result.diagnostic.ok) {
          step.diagnostic = filter_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = filter_result.output_batch.rows.size();
        step.materialized_output_batch = filter_result.output_batch;
        step.mga_statement_context = filter_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveJoinRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    std::vector<api::EngineSqlTruthValue> predicate_truth_values,
    const std::size_t pair_count,
    const std::size_t output_row_bound,
    const exec::CanonicalAcceptedJoinKind join_kind,
    std::string operation_name,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kJoin;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [predicate_truth_values = std::move(predicate_truth_values), pair_count,
       output_row_bound, join_kind, operation_name = std::move(operation_name),
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 || node.input_physical_node_ids.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = operation_name +
              " executor did not receive two typed input batches";
          return step;
        }
        const auto& left_batch = *inputs[0].materialized_output_batch;
        const auto& right_batch = *inputs[1].materialized_output_batch;
        if (left_batch.rows.size() != 0 &&
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() /
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              operation_name + " pair cardinality overflowed";
          return step;
        }
        const auto actual_pair_count =
            left_batch.rows.size() * right_batch.rows.size();
        if (actual_pair_count != pair_count ||
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() -
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = operation_name +
              " input cardinality differs or overflows its selected cost";
          return step;
        }
        exec::CanonicalJoinKindRequest join_request;
        auto& key_request = join_request.residual_request.key_request;
        key_request.physical_dag = dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          // ExecuteCanonicalJoinKind deliberately validates a join-root DAG.
          // Give it the selected join and its exact two inputs as an
          // operator-local view; the outer dispatcher retains and validates
          // the immutable full DAG for FILTER/PROJECT execution and result
          // publication.
          const auto left_input_id = node.input_physical_node_ids[0];
          const auto right_input_id = node.input_physical_node_ids[1];
          std::erase_if(key_request.physical_dag.nodes, [&](const auto& item) {
            return item.physical_node_id != node.physical_node_id &&
                   item.physical_node_id != left_input_id &&
                   item.physical_node_id != right_input_id;
          });
          key_request.physical_dag.root_physical_node_id =
              node.physical_node_id;
        }
        key_request.selected_physical_node_id = node.physical_node_id;
        key_request.left_batch = left_batch;
        key_request.right_batch = right_batch;
        key_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, key_request.physical_dag);
        join_request.residual_request.residual_truth_values =
            predicate_truth_values;
        join_request.residual_request.maximum_candidate_rechecks =
            std::max<std::size_t>(1, pair_count);
        join_request.join_kind = join_kind;
        join_request.bound_pair_truth_profile = true;
        join_request.maximum_output_rows =
            std::max<std::size_t>(1, output_row_bound);
        const auto join_result =
            exec::ExecuteCanonicalJoinKind(join_request);
        if (!join_result.diagnostic.ok) {
          step.diagnostic = join_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            left_batch.rows.size() + right_batch.rows.size();
        step.rows_examined = pair_count;
        step.output_row_count = join_result.output_batch.rows.size();
        step.materialized_output_batch = join_result.output_batch;
        step.mga_statement_context = join_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveSetOperationRegistration(
    std::unordered_map<std::uint64_t, PreparedLiveSetNode>
        prepared_set_nodes,
    std::string implementation_id,
    std::string capability_uuid,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared_set_nodes = std::move(prepared_set_nodes),
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        const auto prepared =
            prepared_set_nodes.find(node.relational_node_id);
        if (prepared == prepared_set_nodes.end() ||
            prepared->second.profile.implementation_id !=
                node.implementation_id ||
            inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
          step.diagnostic.detail =
              "set-operation executor input/profile is unresolved";
          return step;
        }
        const auto& config = prepared->second;
        exec::CanonicalSetOperationAllRequest set_request;
        set_request.physical_dag = dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          std::unordered_set<std::uint64_t> execution_view_nodes;
          std::vector<std::uint64_t> execution_view_pending{
              node.physical_node_id};
          while (!execution_view_pending.empty()) {
            const auto physical_node_id = execution_view_pending.back();
            execution_view_pending.pop_back();
            if (!execution_view_nodes.insert(physical_node_id).second) {
              continue;
            }
            const auto found = std::ranges::find_if(
                dag.nodes, [&](const auto& candidate) {
                  return candidate.physical_node_id == physical_node_id;
                });
            if (found == dag.nodes.end()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
              step.diagnostic.detail =
                  "set-operation execution view is unresolved";
              return step;
            }
            execution_view_pending.insert(
                execution_view_pending.end(),
                found->input_physical_node_ids.begin(),
                found->input_physical_node_ids.end());
          }
          std::erase_if(set_request.physical_dag.nodes,
                        [&](const auto& candidate) {
                          return !execution_view_nodes.contains(
                              candidate.physical_node_id);
                        });
          set_request.physical_dag.root_physical_node_id =
              node.physical_node_id;
        }
        set_request.selected_physical_node_id = node.physical_node_id;
        set_request.left_batch = *inputs[0].materialized_output_batch;
        set_request.right_batch = *inputs[1].materialized_output_batch;
        set_request.result_columns = config.prepared.result_columns;
        set_request.operation = config.profile.operation;
        set_request.alignment = config.profile.alignment;
        set_request.quantifier = config.profile.quantifier;
        set_request.equality_profile = config.profile.equality_profile;
        set_request.type_profile = config.profile.type_profile;
        set_request.collation_bindings =
            config.prepared.collation_bindings;
        set_request.maximum_equality_comparison_count =
            std::max<std::size_t>(
                1, config.maximum_equality_comparison_count);
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, config.maximum_output_row_count);
        set_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
            mga_context, set_request.physical_dag);
        const auto set_result =
            config.profile.quantifier ==
                    exec::CanonicalSetOperationQuantifier::kAll
                ? exec::ExecuteCanonicalSetOperationAll(set_request)
                : exec::ExecuteCanonicalSetOperationDistinct(set_request);
        if (!set_result.diagnostic.ok) {
          step.diagnostic = set_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = set_result.left_input_row_count +
                               set_result.right_input_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = set_result.output_batch.rows.size();
        step.materialized_output_batch = set_result.output_batch;
        step.mga_statement_context = set_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveQueryDistinctRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_value_comparisons,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id =
      "aggregate.query-distinct.typed.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [equality_terms = std::move(equality_terms), maximum_input_row_count,
       maximum_value_comparisons, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-DISTINCT-INPUT-V1";
          step.diagnostic.detail =
              "query DISTINCT did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        exec::CanonicalDescriptorDistinctRequest distinct_request;
        distinct_request.physical_dag = dag;
        distinct_request.selected_physical_node_id = node.physical_node_id;
        distinct_request.input_batch = input_batch;
        distinct_request.equality_terms = equality_terms;
        distinct_request.maximum_value_comparisons =
            maximum_value_comparisons;
        distinct_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto distinct_result =
            exec::ExecuteCanonicalDescriptorDistinct(distinct_request);
        if (!distinct_result.diagnostic.ok) {
          step.diagnostic = distinct_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = distinct_result.output_batch.rows.size();
        step.materialized_output_batch = distinct_result.output_batch;
        step.mga_statement_context = distinct_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveCountStarRegistration(
    exec::ExecutorColumnDescriptor result_column,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.count-star.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [result_column = std::move(result_column), maximum_input_row_count,
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "COUNT(*) did not receive its bounded typed input batch";
          return step;
        }
        exec::CanonicalDescriptorCountRequest aggregate_request;
        aggregate_request.physical_dag = dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          std::unordered_set<std::uint64_t> execution_view_nodes;
          std::vector<std::uint64_t> execution_view_pending{
              node.physical_node_id};
          while (!execution_view_pending.empty()) {
            const auto physical_node_id = execution_view_pending.back();
            execution_view_pending.pop_back();
            if (!execution_view_nodes.insert(physical_node_id).second) {
              continue;
            }
            const auto found = std::ranges::find_if(
                dag.nodes, [&](const auto& candidate) {
                  return candidate.physical_node_id == physical_node_id;
                });
            if (found == dag.nodes.end()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "COUNT(*) execution view is unresolved";
              return step;
            }
            execution_view_pending.insert(
                execution_view_pending.end(),
                found->input_physical_node_ids.begin(),
                found->input_physical_node_ids.end());
          }
          std::erase_if(aggregate_request.physical_dag.nodes,
                        [&](const auto& candidate) {
                          return !execution_view_nodes.contains(
                              candidate.physical_node_id);
                        });
          aggregate_request.physical_dag.root_physical_node_id =
              node.physical_node_id;
        }
        aggregate_request.selected_physical_node_id = node.physical_node_id;
        aggregate_request.input_batch =
            *inputs.front().materialized_output_batch;
        aggregate_request.count_column = result_column;
        aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, aggregate_request.physical_dag);
        const auto aggregate_result =
            exec::ExecuteCanonicalDescriptorCountStar(aggregate_request);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = aggregate_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = aggregate_request.input_batch.rows.size();
        step.rows_examined = step.input_row_count;
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch = aggregate_result.output_batch;
        step.mga_statement_context =
            aggregate_request.mga_authority.statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateRegistryRegistration(
    PreparedGlobalAggregateRoot prepared,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.registry-core.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared = std::move(prepared), maximum_input_row_count,
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate registry did not receive its bounded typed input";
          return step;
        }
        exec::TypedPhysicalNodeDag execution_dag = dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          std::unordered_set<std::uint64_t> execution_view_nodes;
          std::vector<std::uint64_t> execution_view_pending{
              node.physical_node_id};
          while (!execution_view_pending.empty()) {
            const auto physical_node_id = execution_view_pending.back();
            execution_view_pending.pop_back();
            if (!execution_view_nodes.insert(physical_node_id).second) {
              continue;
            }
            const auto found = std::ranges::find_if(
                dag.nodes, [&](const auto& candidate) {
                  return candidate.physical_node_id == physical_node_id;
                });
            if (found == dag.nodes.end()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "aggregate registry execution view is unresolved";
              return step;
            }
            execution_view_pending.insert(
                execution_view_pending.end(),
                found->input_physical_node_ids.begin(),
                found->input_physical_node_ids.end());
          }
          std::erase_if(execution_dag.nodes, [&](const auto& candidate) {
            return !execution_view_nodes.contains(
                candidate.physical_node_id);
          });
          execution_dag.root_physical_node_id = node.physical_node_id;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        std::optional<std::vector<api::EngineSqlTruthValue>>
            filter_truth_values;
        if (prepared.filter_column.has_value()) {
          std::vector<api::EngineSqlTruthValue> materialized_filter;
          std::string filter_detail;
          if (!MaterializeAggregateFilterTruthValues(
                  input_batch, *prepared.filter_column,
                  prepared.filter_descriptor_id, &materialized_filter,
                  &filter_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail = std::move(filter_detail);
            return step;
          }
          filter_truth_values = std::move(materialized_filter);
        }
        exec::CanonicalAggregateRuntimeRequest aggregate_request;
        aggregate_request.physical_dag = std::move(execution_dag);
        aggregate_request.selected_physical_node_id = node.physical_node_id;
        aggregate_request.descriptor = prepared.aggregate_descriptor;
        aggregate_request.input_batch = input_batch;
        aggregate_request.value_columns = prepared.value_columns;
        aggregate_request.value_expression_descriptor_ids =
            prepared.value_descriptor_ids;
        aggregate_request.direct_arguments = prepared.direct_arguments;
        aggregate_request.result_column = prepared.result_column;
        aggregate_request.filter_truth_values =
            std::move(filter_truth_values);
        aggregate_request.distinct = prepared.distinct;
        aggregate_request.aggregate_order_terms =
            prepared.aggregate_order_terms;
        aggregate_request.aggregate_separator =
            prepared.aggregate_separator;
        aggregate_request.listagg_overflow_mode =
            prepared.listagg_overflow_mode;
        aggregate_request.listagg_max_output_bytes =
            prepared.listagg_max_output_bytes;
        aggregate_request.listagg_truncation_indicator =
            prepared.listagg_truncation_indicator;
        aggregate_request.listagg_with_count =
            prepared.listagg_with_count;
        aggregate_request.forced_strategy =
            exec::CanonicalAggregateExecutionStrategy::serial;
        aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, aggregate_request.physical_dag);
        const auto aggregate_result =
            exec::ExecuteCanonicalAggregateRuntime(aggregate_request);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = aggregate_result.diagnostic;
          return step;
        }
        step.authority = aggregate_result.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch = aggregate_result.output_batch;
        step.mga_statement_context =
            aggregate_request.mga_authority.statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveGroupedCountSumRegistration(
    PreparedGroupedCountSumRoot prepared,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_output_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.registry-grouping-sets.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared = std::move(prepared), maximum_input_row_count,
       maximum_output_row_count, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped COUNT/SUM input or projection is invalid";
          return step;
        }
        exec::TypedPhysicalNodeDag execution_dag = dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          std::unordered_set<std::uint64_t> execution_view_nodes;
          std::vector<std::uint64_t> pending{node.physical_node_id};
          while (!pending.empty()) {
            const auto physical_node_id = pending.back();
            pending.pop_back();
            if (!execution_view_nodes.insert(physical_node_id).second) {
              continue;
            }
            const auto found = std::ranges::find_if(
                dag.nodes, [&](const auto& candidate) {
                  return candidate.physical_node_id == physical_node_id;
                });
            if (found == dag.nodes.end()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed grouped aggregate execution view is unresolved";
              return step;
            }
            pending.insert(pending.end(),
                           found->input_physical_node_ids.begin(),
                           found->input_physical_node_ids.end());
          }
          std::erase_if(execution_dag.nodes, [&](const auto& candidate) {
            return !execution_view_nodes.contains(candidate.physical_node_id);
          });
          execution_dag.root_physical_node_id = node.physical_node_id;
        }
        if (!prepared.grouping_projection_columns.empty()) {
          const auto runtime_node = std::ranges::find_if(
              execution_dag.nodes, [&](const auto& candidate) {
                return candidate.physical_node_id == node.physical_node_id;
              });
          if (runtime_node == execution_dag.nodes.end() ||
              runtime_node->output_descriptor_ids.size() !=
                  prepared.key_terms.size() + 2 +
                      prepared.grouping_projection_columns.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouping projection descriptor shape drifted";
            return step;
          }
          runtime_node->output_descriptor_ids.resize(
              prepared.key_terms.size() + 2);
        }
        const auto make_aggregate = [](
                                        const PreparedGlobalAggregateRoot&
                                            prepared_aggregate) {
          exec::CanonicalAggregateRuntimeRequest aggregate;
          aggregate.descriptor = prepared_aggregate.aggregate_descriptor;
          aggregate.value_columns = prepared_aggregate.value_columns;
          aggregate.value_expression_descriptor_ids =
              prepared_aggregate.value_descriptor_ids;
          aggregate.direct_arguments = prepared_aggregate.direct_arguments;
          aggregate.result_column = prepared_aggregate.result_column;
          aggregate.distinct = prepared_aggregate.distinct;
          aggregate.aggregate_order_terms =
              prepared_aggregate.aggregate_order_terms;
          aggregate.aggregate_separator =
              prepared_aggregate.aggregate_separator;
          aggregate.listagg_overflow_mode =
              prepared_aggregate.listagg_overflow_mode;
          aggregate.listagg_max_output_bytes =
              prepared_aggregate.listagg_max_output_bytes;
          aggregate.listagg_truncation_indicator =
              prepared_aggregate.listagg_truncation_indicator;
          aggregate.listagg_with_count =
              prepared_aggregate.listagg_with_count;
          aggregate.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          return aggregate;
        };
        exec::CanonicalGroupedAggregateSetRuntimeRequest grouped_request;
        auto& first = grouped_request.first_aggregate;
        first.aggregate_request = make_aggregate(prepared.count);
        first.aggregate_request.physical_dag = std::move(execution_dag);
        first.aggregate_request.selected_physical_node_id =
            node.physical_node_id;
        first.aggregate_request.input_batch =
            *inputs.front().materialized_output_batch;
        first.aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, first.aggregate_request.physical_dag);
        first.group_key_terms = prepared.key_terms;
        first.group_result_columns = prepared.key_result_columns;
        first.grouping_sets = prepared.grouping_sets;
        first.maximum_group_count = maximum_output_row_count;
        first.maximum_output_rows = maximum_output_row_count;
        auto sum = make_aggregate(prepared.sum);
        sum.mga_authority = first.aggregate_request.mga_authority;
        grouped_request.additional_aggregates = {std::move(sum)};
        auto grouped =
            exec::ExecuteCanonicalGroupedAggregateSetRuntime(grouped_request);
        if (!grouped.diagnostic.ok) {
          step.diagnostic = grouped.diagnostic;
          return step;
        }
        if (!grouped.group_identity_proven ||
            !grouped.shared_state_authority_used ||
            grouped.aggregate_count != 2 ||
            grouped.groups.size() != grouped.output_batch.rows.size() ||
            grouped.output_batch.rows.size() > maximum_output_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped COUNT/SUM shared state is unproven";
          return step;
        }
        std::vector<bool> grouping_sets_observed(
            prepared.grouping_sets.size(), false);
        for (const auto& group : grouped.groups) {
          if (group.grouping_set_ordinal >= prepared.grouping_sets.size() ||
              group.grouping_indicators.size() !=
                  prepared.key_terms.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouped runtime returned invalid metadata";
            return step;
          }
          grouping_sets_observed[group.grouping_set_ordinal] = true;
          const auto expected =
              exec::ComputeCanonicalAggregateGroupingMetadata(
                  prepared.key_terms.size(),
                  prepared.grouping_sets[group.grouping_set_ordinal]);
          if (!expected.diagnostic.ok ||
              group.grouping_indicators != expected.grouping_indicators ||
              group.grouping_id != expected.grouping_id) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouping metadata identity drifted";
            return step;
          }
        }
        if (std::ranges::find(grouping_sets_observed, false) !=
            grouping_sets_observed.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped runtime omitted a grouping set";
          return step;
        }
        if (!prepared.grouping_projection_columns.empty()) {
          if (prepared.grouping_projection_columns.size() !=
                  prepared.key_terms.size() + 1 ||
              grouped.output_batch.columns.size() !=
                  prepared.key_terms.size() + 2) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouping projection result shape drifted";
            return step;
          }
          grouped.output_batch.columns.insert(
              grouped.output_batch.columns.end(),
              prepared.grouping_projection_columns.begin(),
              prepared.grouping_projection_columns.end());
          for (std::size_t group_ordinal = 0;
               group_ordinal < grouped.groups.size(); ++group_ordinal) {
            auto& output_row = grouped.output_batch.rows[group_ordinal];
            const auto& metadata = grouped.groups[group_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  metadata.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              output_row.values.push_back(std::move(indicator));
            }
            if (metadata.grouping_id >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed GROUPING_ID exceeds int64";
              return step;
            }
            api::EngineTypedValue grouping_id;
            grouping_id.descriptor =
                prepared.grouping_projection_columns.back().descriptor;
            grouping_id.encoded_value =
                std::to_string(metadata.grouping_id);
            grouping_id.state = api::EngineValueState::value;
            output_row.values.push_back(std::move(grouping_id));
          }
          const auto projected = exec::ValidateCanonicalDescriptorBatch(
              grouped.output_batch, node.output_descriptor_ids);
          if (!projected.ok) {
            step.diagnostic = projected;
            return step;
          }
        }
        step.authority = grouped.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            first.aggregate_request.input_batch.rows.size();
        step.rows_examined = step.input_row_count;
        step.output_row_count = grouped.output_batch.rows.size();
        step.materialized_output_batch = grouped.output_batch;
        step.mga_statement_context = grouped.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveSortRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> order_terms,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSort;
  registration.implementation_id = "sort.typed.terms.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [order_terms = std::move(order_terms),
       deterministic_tie_evidence_uuid =
           std::move(deterministic_tie_evidence_uuid),
       maximum_input_row_count, maximum_pair_comparisons,
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        exec::CanonicalDescriptorSortRequest sort_request;
        sort_request.physical_dag = dag;
        sort_request.selected_physical_node_id = node.physical_node_id;
        sort_request.input_batch = input_batch;
        sort_request.order_terms = order_terms;
        sort_request.deterministic_tie_evidence_uuid =
            deterministic_tie_evidence_uuid;
        sort_request.maximum_pair_comparisons = maximum_pair_comparisons;
        sort_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto sort_result =
            exec::ExecuteCanonicalDescriptorSort(sort_request);
        if (!sort_result.diagnostic.ok) {
          step.diagnostic = sort_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = sort_result.output_batch.rows.size();
        step.materialized_output_batch = sort_result.output_batch;
        step.mga_statement_context = sort_result.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveExpressionSortRegistration(
    std::vector<exec::CanonicalDescriptorOrderTerm> order_terms,
    std::vector<PreparedSortExpression> expressions,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSort;
  registration.implementation_id = "sort.typed.expression-row.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [order_terms = std::move(order_terms),
       expressions = std::move(expressions),
       deterministic_tie_evidence_uuid =
           std::move(deterministic_tie_evidence_uuid),
       maximum_input_row_count, maximum_pair_comparisons,
       relational_dag = std::move(relational_dag),
       expression_services = std::move(expression_services),
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "expression SORT did not receive its bounded typed input "
              "batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        const auto input_validation = exec::ValidateCanonicalDescriptorBatch(
            input_batch, inputs.front().output_descriptor_ids);
        if (!input_validation.ok) {
          step.diagnostic = input_validation;
          return step;
        }
        const auto mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto before =
            exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                            dag);
        if (!before.ok) {
          step.diagnostic = before;
          return step;
        }
        exec::DescriptorBatch expression_batch;
        std::string expression_detail;
        if (!MaterializeExpressionSortBatch(
                relational_dag, expressions, input_batch,
                expression_services, &expression_batch,
                &expression_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-EXPRESSION-V1";
          step.diagnostic.detail = std::move(expression_detail);
          return step;
        }

        exec::CanonicalDescriptorSortRequest sort_request;
        sort_request.physical_dag = dag;
        sort_request.selected_physical_node_id = node.physical_node_id;
        sort_request.input_batch = input_batch;
        sort_request.order_key_batch = std::move(expression_batch);
        sort_request.order_terms = order_terms;
        sort_request.deterministic_tie_evidence_uuid =
            deterministic_tie_evidence_uuid;
        sort_request.maximum_pair_comparisons = maximum_pair_comparisons;
        sort_request.mga_authority = mga_authority;
        const auto sorted = exec::ExecuteCanonicalDescriptorSort(sort_request);
        if (!sorted.diagnostic.ok) {
          step.diagnostic = sorted.diagnostic;
          return step;
        }
        const auto output_validation =
            exec::ValidateCanonicalDescriptorBatch(
                sorted.output_batch, node.output_descriptor_ids);
        if (!output_validation.ok) {
          step.diagnostic = output_validation;
          return step;
        }
        const auto after =
            exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                            dag);
        if (!after.ok) {
          step.diagnostic = after;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = sorted.output_batch.rows.size();
        step.materialized_output_batch = sorted.output_batch;
        step.mga_statement_context = sorted.mga_statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveLimitRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    const std::uint64_t row_limit,
    const std::uint64_t row_offset,
    const bool fetch_first_rows_only,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kLimit;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [row_limit, row_offset, fetch_first_rows_only,
       maximum_input_row_count, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT/FETCH did not receive its bounded typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        exec::CanonicalDescriptorLimitResult limited;
        if (fetch_first_rows_only) {
          exec::CanonicalDescriptorFetchProfileRequest fetch_request;
          fetch_request.physical_dag = dag;
          fetch_request.selected_physical_node_id = node.physical_node_id;
          fetch_request.input_batch = input_batch;
          fetch_request.form =
              exec::CanonicalFetchTopProfileForm::fetch_first_rows_only;
          fetch_request.row_count = row_limit;
          fetch_request.offset = row_offset;
          fetch_request.row_count_is_bound = true;
          fetch_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context, dag);
          const auto fetched =
              exec::ExecuteCanonicalDescriptorFetchProfile(fetch_request);
          limited.diagnostic = fetched.diagnostic;
          limited.output_batch = fetched.output_batch;
          limited.selected_plan_uuid = fetched.selected_plan_uuid;
          limited.executed_physical_node_id =
              fetched.executed_physical_node_id;
          limited.causal_counter_id = fetched.causal_counter_id;
          limited.mga_statement_context = fetched.mga_statement_context;
        } else {
          exec::CanonicalDescriptorLimitRequest limit_request;
          limit_request.physical_dag = dag;
          limit_request.selected_physical_node_id = node.physical_node_id;
          limit_request.input_batch = input_batch;
          limit_request.limit = row_limit;
          limit_request.offset = row_offset;
          limit_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context, dag);
          limited = exec::ExecuteCanonicalDescriptorLimit(limit_request);
        }
        if (!limited.diagnostic.ok) {
          step.diagnostic = limited.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = limited.output_batch.rows.size();
        step.materialized_output_batch = limited.output_batch;
        step.mga_statement_context = limited.mga_statement_context;
        return step;
      };
  return registration;
}

// QOW-SOURCE-RCP-049-NODE-DRIVEN-COMPOSITION-COMPILER-V1
// Compile a connected object-free relational graph by node contract, rather
// than by a whole-query shape name.  Existing exact profiles remain preferred
// while this compiler grows across the remaining relational node families.
// The compiler admits a descriptor-valid unary tail containing at most one
// FILTER, PROJECT, query DISTINCT, SORT, and LIMIT/FETCH node over either one
// canonical VALUES leaf, a two-VALUES accepted JOIN-kind branch, or an exact
// or losslessly reconciled ordinal/BY NAME quantified set-operation subtree.
// Every node still executes through the ordinary optimizer-published ABI-v2
// DAG and its canonical executor.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() < 3) return result;

  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  auto current = find_node(graph.root_logical_node_id);
  if (current == graph.nodes.end()) return result;

  std::vector<const plan::CanonicalLogicalRelationalNode*> reverse_chain;
  std::unordered_set<std::uint32_t> visited;
  std::unordered_set<plan::CanonicalLogicalRelationalNodeKind> unary_kinds;
  const plan::CanonicalLogicalRelationalNode* join_left_node = nullptr;
  const plan::CanonicalLogicalRelationalNode* join_right_node = nullptr;
  std::optional<exec::CanonicalAcceptedJoinKind> join_kind;
  std::string join_component;
  std::string join_operation_name;
  std::unordered_map<
      std::uint32_t, const plan::CanonicalLogicalRelationalNode*>
      set_base_nodes;
  std::unordered_map<std::uint32_t, LiveSetOperationProfile>
      set_profiles;
  bool registry_aggregate_composable = false;
  LiveUnaryAggregateExpressionProfile global_aggregate_profile;
  LivePairStatisticalExpressionProfile pair_aggregate_profile;
  LiveStringAggregateExpressionProfile string_aggregate_profile;
  LiveOrderedSingleCollectionExpressionProfile
      ordered_collection_profile;
  LiveJsonObjectAggregateExpressionProfile json_object_profile;
  LiveListaggExpressionProfile listagg_profile;
  LiveOrderedSetExpressionProfile ordered_set_profile;
  LiveApproximateExpressionProfile approximate_profile;
  LiveGroupedCountSumProfile grouped_aggregate_profile;
  std::size_t sort_count = 0;
  while (current != graph.nodes.end()) {
    if (!visited.insert(current->logical_node_id).second ||
        !current->required_object_uuids.empty()) {
      return result;
    }
    reverse_chain.push_back(&*current);
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      if (current->semantic_variant_id != "values.literal-table.v1" ||
          !current->input_logical_node_ids.empty()) {
        return result;
      }
      break;
    }
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kJoin) {
      if (current->semantic_variant_id == "join.inner.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kInner;
        join_component = "inner";
        join_operation_name = "INNER JOIN";
      } else if (current->semantic_variant_id == "join.cross.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kCross;
        join_component = "cross";
        join_operation_name = "CROSS JOIN";
      } else if (current->semantic_variant_id == "join.left-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
        join_component = "left-outer";
        join_operation_name = "LEFT OUTER JOIN";
      } else if (current->semantic_variant_id == "join.right-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
        join_component = "right-outer";
        join_operation_name = "RIGHT OUTER JOIN";
      } else if (current->semantic_variant_id == "join.full-outer.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
        join_component = "full-outer";
        join_operation_name = "FULL OUTER JOIN";
      } else if (current->semantic_variant_id == "join.left-semi.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
        join_component = "left-semi";
        join_operation_name = "LEFT SEMI JOIN";
      } else if (current->semantic_variant_id == "join.left-anti.v1") {
        join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
        join_component = "left-anti";
        join_operation_name = "LEFT ANTI JOIN";
      } else {
        return result;
      }
      const auto expected_expression_count =
          *join_kind == exec::CanonicalAcceptedJoinKind::kCross ? 0U : 1U;
      if (current->input_logical_node_ids.size() != 2 ||
          current->input_logical_node_ids[0] ==
              current->input_logical_node_ids[1] ||
          current->bound_expression_ids.size() != expected_expression_count ||
          !current->required_property_uuids.empty() ||
          !current->delivered_property_uuids.empty()) {
        return result;
      }
      const auto left = find_node(current->input_logical_node_ids[0]);
      const auto right = find_node(current->input_logical_node_ids[1]);
      if (left == graph.nodes.end() || right == graph.nodes.end() ||
          left->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          right->node_kind !=
              plan::CanonicalLogicalRelationalNodeKind::kValues ||
          left->semantic_variant_id != "values.literal-table.v1" ||
          right->semantic_variant_id != "values.literal-table.v1" ||
          !left->input_logical_node_ids.empty() ||
          !right->input_logical_node_ids.empty() ||
          !left->required_object_uuids.empty() ||
          !right->required_object_uuids.empty() ||
          !left->required_property_uuids.empty() ||
          !right->required_property_uuids.empty() ||
          !left->delivered_property_uuids.empty() ||
          !right->delivered_property_uuids.empty() ||
          !visited.insert(left->logical_node_id).second ||
          !visited.insert(right->logical_node_id).second) {
        return result;
      }
      join_left_node = &*left;
      join_right_node = &*right;
      break;
    }
    if (current->node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
      std::vector<std::uint64_t> pending_set_base{
          current->logical_node_id};
      while (!pending_set_base.empty()) {
        const auto node_id = pending_set_base.back();
        pending_set_base.pop_back();
        if (set_base_nodes.contains(node_id)) continue;
        const auto node = find_node(node_id);
        if (node == graph.nodes.end() ||
            !node->required_object_uuids.empty() ||
            !node->required_property_uuids.empty() ||
            !node->delivered_property_uuids.empty() ||
            (node_id != current->logical_node_id &&
             visited.contains(node_id))) {
          return result;
        }
        set_base_nodes.emplace(node_id, &*node);
        if (node->node_kind ==
            plan::CanonicalLogicalRelationalNodeKind::kValues) {
          if (node->semantic_variant_id != "values.literal-table.v1" ||
              !node->input_logical_node_ids.empty()) {
            return result;
          }
          continue;
        }
        if (node->node_kind !=
            plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
          return result;
        }
        auto profile =
            MatchLiveSetOperationProfile(node->semantic_variant_id);
        if (!profile.matched ||
            (profile.alignment !=
                 exec::CanonicalSetOperationAlignment::kOrdinal &&
             profile.alignment !=
                 exec::CanonicalSetOperationAlignment::kByName) ||
            (profile.type_profile !=
                 exec::CanonicalSetOperationTypeProfile::kExact &&
             profile.type_profile !=
                 exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) ||
            (profile.equality_profile !=
                 exec::CanonicalSetOperationEqualityProfile::kExactTyped &&
             profile.equality_profile !=
                 exec::CanonicalSetOperationEqualityProfile::
                     kNullEqualBoundCollation) ||
            node->input_logical_node_ids.size() != 2 ||
            node->input_logical_node_ids[0] ==
                node->input_logical_node_ids[1] ||
            !node->bound_expression_ids.empty()) {
          return result;
        }
        set_profiles.emplace(node_id, std::move(profile));
        pending_set_base.insert(pending_set_base.end(),
                                node->input_logical_node_ids.begin(),
                                node->input_logical_node_ids.end());
      }
      for (const auto& [node_id, node] : set_base_nodes) {
        (void)node;
        if (node_id != current->logical_node_id &&
            !visited.insert(node_id).second) {
          return result;
        }
      }
      break;
    }
    if (current->input_logical_node_ids.size() != 1 ||
        !unary_kinds.insert(current->node_kind).second) {
      return result;
    }
    switch (current->node_kind) {
      case plan::CanonicalLogicalRelationalNodeKind::kFilter:
        if ((current->semantic_variant_id != "filter.where.v1" &&
             !IsLiveGroupedHavingProfile(
                 current->semantic_variant_id)) ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kProject:
        if (current->semantic_variant_id != "project.select-list.v1" ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kAggregate:
        global_aggregate_profile =
            MatchLiveUnaryAggregateExpressionProfile(
                current->semantic_variant_id);
        pair_aggregate_profile =
            MatchLivePairStatisticalExpressionProfile(
                current->semantic_variant_id);
        string_aggregate_profile =
            MatchLiveStringAggregateExpressionProfile(
                current->semantic_variant_id);
        ordered_collection_profile =
            MatchLiveOrderedSingleCollectionExpressionProfile(
                current->semantic_variant_id);
        json_object_profile =
            MatchLiveJsonObjectAggregateExpressionProfile(
                current->semantic_variant_id);
        listagg_profile = MatchLiveListaggExpressionProfile(
            current->semantic_variant_id);
        ordered_set_profile = MatchLiveOrderedSetExpressionProfile(
            current->semantic_variant_id);
        approximate_profile = MatchLiveApproximateExpressionProfile(
            current->semantic_variant_id);
        grouped_aggregate_profile =
            MatchLiveGroupedCountSumProfile(current->semantic_variant_id);
        registry_aggregate_composable =
            global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::count ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::sum ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::avg ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::min ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::max ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_and ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_or ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::every);
        if (global_aggregate_profile.matched) {
          const auto function = global_aggregate_profile.function;
          registry_aggregate_composable =
              registry_aggregate_composable ||
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::variance_pop ||
              function == exec::CanonicalAggregateFunction::stddev ||
              function == exec::CanonicalAggregateFunction::variance ||
              function == exec::CanonicalAggregateFunction::stddev_samp ||
              function == exec::CanonicalAggregateFunction::variance_samp;
        }
        registry_aggregate_composable =
            registry_aggregate_composable || pair_aggregate_profile.matched ||
            string_aggregate_profile.matched ||
            ordered_collection_profile.matched ||
            json_object_profile.matched || listagg_profile.matched ||
            ordered_set_profile.matched || approximate_profile.matched ||
            grouped_aggregate_profile.matched;
        if ((current->semantic_variant_id !=
                 "aggregate.query-distinct.v1" &&
             !registry_aggregate_composable) ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kSort:
        ++sort_count;
        if (current->semantic_variant_id != "sort.required-order.v1" ||
            current->required_property_uuids.size() != 1 ||
            current->delivered_property_uuids.size() != 1) {
          return result;
        }
        break;
      case plan::CanonicalLogicalRelationalNodeKind::kLimit:
        if ((current->semantic_variant_id != "limit.bound-count.v1" &&
             current->semantic_variant_id !=
                 "limit.bound-count-offset.v1" &&
             current->semantic_variant_id !=
                 "fetch.first-rows-only-offset.v1") ||
            !current->required_property_uuids.empty() ||
            !current->delivered_property_uuids.empty()) {
          return result;
        }
        break;
      default:
        return result;
    }
    current = find_node(current->input_logical_node_ids.front());
  }
  if (reverse_chain.empty() ||
      (reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kValues &&
       reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kJoin &&
       reverse_chain.back()->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kSetOperation) ||
      visited.size() != graph.nodes.size() || sort_count > 1 ||
      (sort_count == 0 &&
       !request.optimizer_request.logical_properties.properties.empty()) ||
      (sort_count == 1 &&
       request.optimizer_request.logical_properties.properties.size() != 1)) {
    return result;
  }
  std::ranges::reverse(reverse_chain);

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };
  constexpr std::string_view kPayloadDiagnostic =
      "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-ADMISSION-V1",
        "node-driven composition lacks optimizer admission");
  }

  MaterializedValues state;
  std::optional<MaterializedValues> join_left_values;
  std::optional<MaterializedValues> join_right_values;
  std::optional<PreparedJoinRoot> prepared_join;
  std::vector<api::EngineSqlTruthValue> join_truth_values;
  std::size_t join_pair_count = 0;
  std::size_t join_output_row_bound = 0;
  std::string join_implementation_id;
  std::unordered_map<std::uint32_t, MaterializedValues>
      set_materialized_values;
  std::unordered_map<std::uint64_t, PreparedLiveSetNode>
      prepared_set_nodes;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);

  std::optional<PreparedFilterRoot> prepared_filter;
  std::vector<api::EngineSqlTruthValue> filter_truth_values;
  std::size_t filter_input_row_count = 0;
  std::optional<PreparedProjectRoot> prepared_project;
  std::size_t project_input_row_count = 0;
  std::string project_implementation_id;
  std::optional<PreparedDistinctRoot> prepared_distinct;
  std::size_t distinct_input_row_count = 0;
  std::size_t distinct_comparison_bound = 0;
  std::optional<PreparedGlobalAggregateRoot> prepared_count_star;
  std::size_t count_star_input_row_count = 0;
  std::optional<PreparedGlobalAggregateRoot> prepared_registry_aggregate;
  std::size_t registry_aggregate_input_row_count = 0;
  std::optional<PreparedGroupedCountSumRoot> prepared_grouped_aggregate;
  std::size_t grouped_aggregate_input_row_count = 0;
  std::size_t grouped_aggregate_output_row_bound = 0;
  std::optional<PreparedSortRoot> prepared_sort;
  std::size_t sort_input_row_count = 0;
  std::size_t sort_comparison_bound = 0;
  std::optional<PreparedLimitRoot> prepared_limit;
  std::size_t limit_input_row_count = 0;
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  bool fetch_first_rows_only = false;
  std::string limit_implementation_id;
  bool planning_values_exact = true;

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.join.capability");
  std::unordered_map<std::string, std::string> set_capability_uuids;
  for (const auto& [node_id, profile] : set_profiles) {
    (void)node_id;
    set_capability_uuids.try_emplace(
        profile.implementation_id,
        DerivedCanonicalUuid(
            identity_scope,
            "composition.set." + profile.implementation_id +
                ".capability"));
  }
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.project.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.distinct.capability");
  const auto count_star_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.count-star.capability");
  const auto registry_aggregate_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.aggregate-registry.capability");
  const auto grouped_aggregate_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "composition.grouped-aggregate.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.sort.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "composition.limit.capability");

  std::vector<LivePhysicalNodeProfile> profiles;
  std::uint64_t total_work = 0;
  if (reverse_chain.front()->node_kind ==
      plan::CanonicalLogicalRelationalNodeKind::kValues) {
    state = MaterializeValues(request.relational_dag,
                              *reverse_chain.front(),
                              request.expression_services);
    if (!state.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "composition VALUES: " + state.detail);
    }
    std::uint64_t values_memory = 1;
    if (!AddBatchMemoryBytes(state.batch, &values_memory) ||
        values_memory >
            request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition VALUES exceeds the admitted memory budget");
    }
    profiles.push_back(
        {reverse_chain.front()->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", state.batch.rows.size(),
         values_memory, 0, 0});
    total_work = state.batch.rows.size();
  } else if (reverse_chain.front()->node_kind ==
             plan::CanonicalLogicalRelationalNodeKind::kJoin) {
    join_left_values = MaterializeValues(
        request.relational_dag, *join_left_node,
        request.expression_services);
    join_right_values = MaterializeValues(
        request.relational_dag, *join_right_node,
        request.expression_services);
    if (!join_left_values->ok || !join_right_values->ok) {
      return refuse(
          std::string(kPayloadDiagnostic),
          !join_left_values->ok
              ? "composition JOIN left VALUES: " + join_left_values->detail
              : "composition JOIN right VALUES: " +
                    join_right_values->detail);
    }
    prepared_join = PrepareJoinRoot(
        request.relational_dag, *reverse_chain.front(), *join_left_node,
        *join_right_node, *join_left_values, *join_right_values, *join_kind);
    if (!prepared_join->ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_join->detail);
    }

    const auto left_count = join_left_values->batch.rows.size();
    const auto right_count = join_right_values->batch.rows.size();
    if (left_count != 0 &&
        right_count > std::numeric_limits<std::size_t>::max() / left_count) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition JOIN pair cardinality overflowed");
    }
    join_pair_count = left_count * right_count;
    join_truth_values.reserve(join_pair_count);
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kCross) {
      join_truth_values.assign(
          join_pair_count, api::EngineSqlTruthValue::true_value);
    } else {
      std::vector<api::EngineTypedValue> predicate_row_values;
      predicate_row_values.reserve(
          join_left_values->batch.columns.size() +
          join_right_values->batch.columns.size());
      for (const auto& left_row : join_left_values->batch.rows) {
        for (const auto& right_row : join_right_values->batch.rows) {
          predicate_row_values.clear();
          predicate_row_values.insert(predicate_row_values.end(),
                                      left_row.values.begin(),
                                      left_row.values.end());
          predicate_row_values.insert(predicate_row_values.end(),
                                      right_row.values.begin(),
                                      right_row.values.end());
          api::EngineSqlTruthValue truth =
              api::EngineSqlTruthValue::unknown;
          std::string detail;
          if (!expression_runtime.EvaluatePredicateForConsumer(
                  prepared_join->predicate_expression_id,
                  prepared_join->predicate_row_binding,
                  predicate_row_values,
                  api::EngineCanonicalExpressionConsumer::join, &truth,
                  &detail)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition JOIN predicate pair " +
                    std::to_string(join_truth_values.size()) + ": " +
                    detail);
          }
          join_truth_values.push_back(truth);
        }
      }
    }

    std::vector<std::size_t> accepted_pairs;
    std::vector<bool> matched_left(left_count, false);
    std::vector<bool> matched_right(right_count, false);
    for (std::size_t pair = 0; pair < join_truth_values.size(); ++pair) {
      if (join_truth_values[pair] !=
          api::EngineSqlTruthValue::true_value) {
        continue;
      }
      accepted_pairs.push_back(pair);
      matched_left[pair / right_count] = true;
      matched_right[pair % right_count] = true;
    }

    state.ok = true;
    const bool left_only =
        *join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti;
    state.batch.columns = join_left_values->batch.columns;
    if (!left_only) {
      state.batch.columns.insert(state.batch.columns.end(),
                                 join_right_values->batch.columns.begin(),
                                 join_right_values->batch.columns.end());
    }
    const auto left_width = join_left_values->batch.columns.size();
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
      for (std::size_t column = 0; column < left_width; ++column) {
        state.batch.columns[column].nullable = true;
      }
    }
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
        *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
      for (std::size_t column = left_width;
           column < state.batch.columns.size(); ++column) {
        state.batch.columns[column].nullable = true;
      }
    }
    const auto append_joined = [&](const std::size_t left_ordinal,
                                   const std::size_t right_ordinal) {
      exec::DescriptorTuple row =
          join_left_values->batch.rows[left_ordinal];
      const auto& right_values =
          join_right_values->batch.rows[right_ordinal].values;
      row.values.insert(row.values.end(), right_values.begin(),
                        right_values.end());
      state.batch.rows.push_back(std::move(row));
    };
    const auto append_unmatched_left = [&](const std::size_t left_ordinal) {
      exec::DescriptorTuple row =
          join_left_values->batch.rows[left_ordinal];
      for (const auto& column : join_right_values->batch.columns) {
        api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = api::EngineValueState::sql_null;
        row.values.push_back(std::move(null_value));
      }
      state.batch.rows.push_back(std::move(row));
    };
    const auto append_unmatched_right = [&](const std::size_t right_ordinal) {
      exec::DescriptorTuple row;
      for (const auto& column : join_left_values->batch.columns) {
        api::EngineTypedValue null_value;
        null_value.descriptor = column.descriptor;
        null_value.is_null = true;
        null_value.state = api::EngineValueState::sql_null;
        row.values.push_back(std::move(null_value));
      }
      const auto& right_values =
          join_right_values->batch.rows[right_ordinal].values;
      row.values.insert(row.values.end(), right_values.begin(),
                        right_values.end());
      state.batch.rows.push_back(std::move(row));
    };

    if (left_only) {
      const bool emit_matches =
          *join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi;
      for (std::size_t left = 0; left < left_count; ++left) {
        if (matched_left[left] == emit_matches) {
          state.batch.rows.push_back(join_left_values->batch.rows[left]);
        }
      }
    } else if (*join_kind ==
               exec::CanonicalAcceptedJoinKind::kRightOuter) {
      for (std::size_t right = 0; right < right_count; ++right) {
        bool emitted = false;
        for (const auto pair : accepted_pairs) {
          if (pair % right_count == right) {
            append_joined(pair / right_count, right);
            emitted = true;
          }
        }
        if (!emitted) append_unmatched_right(right);
      }
    } else if (*join_kind ==
                   exec::CanonicalAcceptedJoinKind::kLeftOuter ||
               *join_kind ==
                   exec::CanonicalAcceptedJoinKind::kFullOuter) {
      std::size_t accepted = 0;
      for (std::size_t left = 0; left < left_count; ++left) {
        bool emitted = false;
        while (accepted < accepted_pairs.size() &&
               accepted_pairs[accepted] / right_count == left) {
          append_joined(left, accepted_pairs[accepted] % right_count);
          ++accepted;
          emitted = true;
        }
        if (!emitted) append_unmatched_left(left);
      }
      if (*join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) {
        for (std::size_t right = 0; right < right_count; ++right) {
          if (!matched_right[right]) append_unmatched_right(right);
        }
      }
    } else {
      for (const auto pair : accepted_pairs) {
        append_joined(pair / right_count, pair % right_count);
      }
    }
    state.result_bindings = prepared_join->result_bindings;
    join_output_row_bound = state.batch.rows.size();
    join_implementation_id =
        "join." + join_component + ".3vl.nested.v1";

    std::uint64_t left_memory = 1;
    std::uint64_t right_memory = 1;
    std::uint64_t join_memory = 1;
    std::uint64_t truth_memory = 0;
    if (!AddBatchMemoryBytes(join_left_values->batch, &left_memory) ||
        !AddBatchMemoryBytes(join_right_values->batch, &right_memory) ||
        !AddBatchMemoryBytes(state.batch, &join_memory) ||
        !CheckedMultiply(join_pair_count,
                         sizeof(api::EngineSqlTruthValue), &truth_memory) ||
        !CheckedAdd(join_memory, left_memory, &join_memory) ||
        !CheckedAdd(join_memory, right_memory, &join_memory) ||
        !CheckedAdd(join_memory, truth_memory, &join_memory) ||
        join_memory >
            request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition JOIN exceeds the admitted memory budget");
    }
    profiles.push_back(
        {join_left_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", left_count, left_memory, 0, 0});
    profiles.push_back(
        {join_right_node->logical_node_id,
         std::string(kValuesImplementationId), values_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kValues,
         exec::PhysicalNodeKind::kValues,
         "canonical.values.materialize.v1", right_count, right_memory, 0, 0});
    profiles.push_back(
        {reverse_chain.front()->logical_node_id, join_implementation_id,
         join_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kJoin,
         exec::PhysicalNodeKind::kJoin,
         "canonical." + join_implementation_id, join_output_row_bound,
         join_memory, 2, 2});
    if (!CheckedAdd(left_count, right_count, &total_work) ||
        !CheckedAdd(total_work, join_pair_count, &total_work) ||
        total_work >
            request.optimizer_request.resource.maximum_candidate_count) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition JOIN work exceeds the admitted bound");
    }
  } else {
    for (const auto& [node_id, node] : set_base_nodes) {
      if (node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues) {
        continue;
      }
      auto materialized = MaterializeValues(
          request.relational_dag, *node, request.expression_services);
      if (!materialized.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      "composition SET VALUES: " + materialized.detail);
      }
      std::uint64_t values_memory = 1;
      if (!AddBatchMemoryBytes(materialized.batch, &values_memory) ||
          values_memory >
              request.optimizer_request.resource.memory_budget_bytes ||
          !CheckedAdd(total_work, materialized.batch.rows.size(),
                      &total_work) ||
          total_work >
              request.optimizer_request.resource.maximum_candidate_count) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                      "composition SET VALUES exceeds an admitted bound");
      }
      profiles.push_back(
          {node_id, std::string(kValuesImplementationId),
           values_capability_uuid,
           plan::CanonicalLogicalRelationalNodeKind::kValues,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", materialized.batch.rows.size(),
           values_memory, 0, 0});
      set_materialized_values.emplace(node_id, std::move(materialized));
    }

    std::unordered_set<std::uint32_t> pending_set_nodes;
    for (const auto& [node_id, profile] : set_profiles) {
      (void)profile;
      pending_set_nodes.insert(node_id);
    }
    while (!pending_set_nodes.empty()) {
      bool progressed = false;
      for (auto pending = pending_set_nodes.begin();
           pending != pending_set_nodes.end();) {
        const auto node_id = *pending;
        const auto* node = set_base_nodes.at(node_id);
        const auto left = set_materialized_values.find(
            node->input_logical_node_ids[0]);
        const auto right = set_materialized_values.find(
            node->input_logical_node_ids[1]);
        if (left == set_materialized_values.end() ||
            right == set_materialized_values.end()) {
          ++pending;
          continue;
        }
        auto prepared = PrepareSetOperationRoot(
            request.context, request.relational_dag, *node, left->second,
            right->second, set_profiles.at(node_id));
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        auto materialized = MaterializeSetOperationPlanningState(
            prepared, set_profiles.at(node_id), left->second, right->second);
        if (!materialized.values.ok ||
            materialized.output_bound >
                std::numeric_limits<std::size_t>::max() ||
            materialized.comparison_bound >
                std::numeric_limits<std::size_t>::max()) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
              materialized.values.detail.empty()
                  ? "composition SET row/comparison bound overflowed"
                  : materialized.values.detail);
        }

        std::uint64_t left_memory = 1;
        std::uint64_t right_memory = 1;
        std::uint64_t set_memory = 1;
        if (!AddBatchMemoryBytes(left->second.batch, &left_memory) ||
            !AddBatchMemoryBytes(right->second.batch, &right_memory) ||
            !AddBatchMemoryBytes(materialized.values.batch, &set_memory) ||
            !CheckedAdd(set_memory, left_memory, &set_memory) ||
            !CheckedAdd(set_memory, right_memory, &set_memory) ||
            !CheckedAdd(set_memory, materialized.work_bound, &set_memory) ||
            set_memory >
                request.optimizer_request.resource.memory_budget_bytes ||
            !CheckedAdd(total_work, materialized.work_bound, &total_work) ||
            total_work >
                request.optimizer_request.resource.maximum_candidate_count) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                        "composition SET exceeds an admitted bound");
        }

        const auto& profile = set_profiles.at(node_id);
        profiles.push_back(
            {node_id, profile.implementation_id,
             set_capability_uuids.at(profile.implementation_id),
             plan::CanonicalLogicalRelationalNodeKind::kSetOperation,
             exec::PhysicalNodeKind::kSetOperation,
             profile.physical_semantic_id,
             static_cast<std::size_t>(materialized.output_bound), set_memory,
             2, 2});
        prepared_set_nodes.emplace(
            node_id,
            PreparedLiveSetNode{
                profile, std::move(prepared),
                static_cast<std::size_t>(materialized.output_bound),
                static_cast<std::size_t>(std::max<std::uint64_t>(
                    1, materialized.comparison_bound))});
        set_materialized_values.emplace(
            node_id, std::move(materialized.values));
        pending = pending_set_nodes.erase(pending);
        progressed = true;
      }
      if (!progressed) {
        return refuse(std::string(kPayloadDiagnostic),
                      "composition SET subtree is cyclic or unresolved");
      }
    }
    state = set_materialized_values.at(
        reverse_chain.front()->logical_node_id);
  }
  const auto add_work = [&](const std::uint64_t work) {
    return CheckedAdd(total_work, work, &total_work) &&
           total_work <=
               request.optimizer_request.resource.maximum_candidate_count;
  };
  for (std::size_t index = 1; index < reverse_chain.size(); ++index) {
    const auto& node = *reverse_chain[index];
    const auto& input_node = *reverse_chain[index - 1];
    const auto input_batch = state.batch;
    const auto input_bindings = state.result_bindings;
    const auto input_row_count = input_batch.rows.size();
    std::uint64_t input_memory = 1;
    if (!AddBatchMemoryBytes(input_batch, &input_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "composition input memory overflowed");
    }

    std::string implementation_id;
    std::string capability_uuid;
    std::string transformation_rule;
    exec::PhysicalNodeKind physical_kind = exec::PhysicalNodeKind::kValues;
    std::uint64_t auxiliary_memory = 0;
    std::vector<std::string> required_property_uuids;
    std::vector<std::string> delivered_property_uuids;
    std::vector<plan::CanonicalLogicalPropertyKind> property_kinds;

    if (!planning_values_exact &&
        node.node_kind !=
            plan::CanonicalLogicalRelationalNodeKind::kLimit) {
      return refuse(
          std::string(kPayloadDiagnostic),
          "composed aggregate planning placeholder only admits a direct "
          "LIMIT/FETCH consumer");
    }

    switch (node.node_kind) {
      case plan::CanonicalLogicalRelationalNodeKind::kFilter: {
        PreparedFilterRoot prepared;
        if (IsLiveGroupedHavingProfile(node.semantic_variant_id)) {
          if (!prepared_grouped_aggregate.has_value() ||
              input_node.node_kind !=
                  plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
              input_node.input_logical_node_ids.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composed HAVING lacks its grouped aggregate");
          }
          const auto aggregate_input = std::ranges::find_if(
              graph.nodes, [&](const auto& candidate) {
                return candidate.logical_node_id ==
                       input_node.input_logical_node_ids.front();
              });
          if (aggregate_input == graph.nodes.end()) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composed HAVING aggregate input is unresolved");
          }
          const auto having = PrepareGroupedHavingRoot(
              request.relational_dag, node, input_node, *aggregate_input,
              *prepared_grouped_aggregate);
          if (!having.ok ||
              having.output_column_count != state.batch.columns.size()) {
            return refuse(std::string(kPayloadDiagnostic), having.detail);
          }
          prepared.predicate_expression_id =
              having.predicate_expression_id;
          prepared.predicate_row_binding = having.row_binding;
          prepared.result_bindings = state.result_bindings;
          prepared.ok = true;
        } else {
          prepared = PrepareFilterRoot(request.relational_dag, node,
                                       input_node, state);
        }
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::vector<api::EngineSqlTruthValue> truth_values;
        truth_values.reserve(input_row_count);
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(input_row_count);
        for (const auto& row : input_batch.rows) {
          api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
          std::string detail;
          if (!expression_runtime.EvaluatePredicateForConsumer(
                  prepared.predicate_expression_id,
                  prepared.predicate_row_binding, row.values,
                  api::EngineCanonicalExpressionConsumer::filter, &truth,
                  &detail)) {
            return refuse(std::string(kPayloadDiagnostic),
                          "FILTER row " +
                              std::to_string(truth_values.size()) + ": " +
                              detail);
          }
          truth_values.push_back(truth);
          if (truth == api::EngineSqlTruthValue::true_value) {
            output.rows.push_back(row);
          }
        }
        if (!add_work(input_row_count)) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                        "composition FILTER work exceeds the admitted bound");
        }
        prepared_filter = std::move(prepared);
        filter_truth_values = std::move(truth_values);
        filter_input_row_count = input_row_count;
        state.batch = std::move(output);
        state.result_bindings = input_bindings;
        implementation_id = "filter.3vl.row.v1";
        capability_uuid = filter_capability_uuid;
        transformation_rule = "canonical.filter.composed-row.3vl.v1";
        physical_kind = exec::PhysicalNodeKind::kFilter;
        if (!CheckedMultiply(input_row_count,
                             sizeof(api::EngineSqlTruthValue),
                             &auxiliary_memory)) {
          return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                        "composition FILTER state memory overflowed");
        }
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kProject: {
        auto prepared = node.bound_expression_ids.empty()
            ? PrepareDescriptorDirectProjectRoot(
                  request.relational_dag, node, input_node, state)
            : PrepareExpressionProjectRoot(
                  request.relational_dag, node, input_node, state,
                  request.expression_services);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        exec::DescriptorBatch output;
        if (prepared.expression_projection) {
          output = prepared.expression_output_batch;
          std::uint64_t work = 0;
          if (!CheckedMultiply(input_row_count,
                               prepared.expressions.size(), &work) ||
              !add_work(work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition PROJECT work exceeds the admitted bound");
          }
        } else {
          output.columns.reserve(prepared.projected_columns.size());
          for (const auto column : prepared.projected_columns) {
            output.columns.push_back(input_batch.columns[column]);
          }
          output.rows.reserve(input_row_count);
          for (const auto& row : input_batch.rows) {
            exec::DescriptorTuple projected;
            projected.values.reserve(prepared.projected_columns.size());
            for (const auto column : prepared.projected_columns) {
              projected.values.push_back(row.values[column]);
            }
            output.rows.push_back(std::move(projected));
          }
          if (!add_work(input_row_count)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition PROJECT work exceeds the admitted bound");
          }
        }
        prepared_project = std::move(prepared);
        project_input_row_count = input_row_count;
        project_implementation_id =
            prepared_project->expression_projection
                ? "project.typed.expression-row.v1"
                : "project.typed.row.v1";
        state.batch = std::move(output);
        state.result_bindings = prepared_project->result_bindings;
        implementation_id = project_implementation_id;
        capability_uuid = project_capability_uuid;
        transformation_rule = prepared_project->expression_projection
            ? "canonical.project.composed-expression-row.v1"
            : "canonical.project.composed-descriptor-row.v1";
        physical_kind = exec::PhysicalNodeKind::kProject;
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kAggregate: {
        if (node.semantic_variant_id ==
            "aggregate.global-count-star.v1") {
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::count, true, false, false);
          if (!prepared.ok || !add_work(input_row_count)) {
            return refuse(
                prepared.ok
                    ? "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1"
                    : std::string(kPayloadDiagnostic),
                prepared.ok
                    ? "composition COUNT(*) work exceeds the admitted bound"
                    : prepared.detail);
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue count;
          count.descriptor = prepared.result_column.descriptor;
          count.encoded_value = std::to_string(input_row_count);
          count.is_null = false;
          count.state = api::EngineValueState::value;
          tuple.values.push_back(std::move(count));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "COUNT(*) planning state: " + canonical.detail
                    : "COUNT(*) planning state: " + values.detail);
          }
          prepared_count_star = std::move(prepared);
          count_star_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_count_star->result_bindings;
          implementation_id = "aggregate.count-star.v1";
          capability_uuid = count_star_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-count-star.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory =
              std::numeric_limits<std::int64_t>::digits10 + 2;
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::count) {
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::count, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          std::size_t count_value = 0;
          const auto value_column = prepared.value_columns.front();
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (prepared.distinct) {
              std::string key = value.descriptor.canonical_type_name + ":" +
                                value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            ++count_value;
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition COUNT(DISTINCT) work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition COUNT(expression) work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue count;
          count.descriptor = prepared.result_column.descriptor;
          count.encoded_value = std::to_string(count_value);
          count.is_null = false;
          count.state = api::EngineValueState::value;
          tuple.values.push_back(std::move(count));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "COUNT(expression) planning state: " + canonical.detail
                    : "COUNT(expression) planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-count-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev_pop ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance_pop ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::stddev_samp ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::variance_samp)) {
          const auto function = global_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          std::uint64_t non_null_count = 0;
          long double numeric_mean = 0.0L;
          long double numeric_m2 = 0.0L;
          const auto value_column = prepared.value_columns.front();
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (value.descriptor.canonical_type_name != "int64") {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition statistical input is not canonical int64");
            }
            if (prepared.distinct) {
              std::string key = value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            std::int64_t decoded = 0;
            const auto [end, error] = std::from_chars(
                value.encoded_value.data(),
                value.encoded_value.data() + value.encoded_value.size(),
                decoded);
            if (error != std::errc{} ||
                end != value.encoded_value.data() +
                           value.encoded_value.size() ||
                non_null_count == std::numeric_limits<std::uint64_t>::max()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition statistical input or count is invalid");
            }
            ++non_null_count;
            const auto numeric = static_cast<long double>(decoded);
            const auto count = static_cast<long double>(non_null_count);
            const auto delta = numeric - numeric_mean;
            numeric_mean += delta / count;
            const auto delta2 = numeric - numeric_mean;
            numeric_m2 += delta * delta2;
            if (!std::isfinite(static_cast<double>(numeric_mean)) ||
                !std::isfinite(static_cast<double>(numeric_m2))) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition statistical state overflowed");
            }
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition statistical DISTINCT work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition statistical work exceeds the admitted bound");
          }
          const bool population =
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::variance_pop;
          const bool deviation =
              function == exec::CanonicalAggregateFunction::stddev_pop ||
              function == exec::CanonicalAggregateFunction::stddev ||
              function == exec::CanonicalAggregateFunction::stddev_samp;
          const bool has_result = non_null_count >= (population ? 1U : 2U);
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue statistic_value;
          statistic_value.descriptor = prepared.result_column.descriptor;
          statistic_value.is_null = !has_result;
          statistic_value.state = has_result
                                      ? api::EngineValueState::value
                                      : api::EngineValueState::sql_null;
          if (has_result) {
            const auto denominator = static_cast<long double>(
                population ? non_null_count : non_null_count - 1);
            auto statistic = numeric_m2 / denominator;
            if (statistic < 0.0L && statistic > -1e-18L) statistic = 0.0L;
            if (deviation) statistic = std::sqrt(statistic);
            if (!std::isfinite(static_cast<double>(statistic))) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition statistical result overflowed");
            }
            std::ostringstream encoded;
            encoded << std::setprecision(17)
                    << static_cast<double>(statistic);
            statistic_value.encoded_value = encoded.str();
          }
          tuple.values.push_back(std::move(statistic_value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "statistical planning state: " + canonical.detail
                    : "statistical planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              global_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::avg) {
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::avg, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          long double real_sum = 0.0L;
          std::uint64_t non_null_count = 0;
          const auto value_column = prepared.value_columns.front();
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (value.descriptor.canonical_type_name != "int64") {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG input is not canonical int64");
            }
            if (prepared.distinct) {
              std::string key = value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            std::int64_t decoded = 0;
            const auto [end, error] = std::from_chars(
                value.encoded_value.data(),
                value.encoded_value.data() + value.encoded_value.size(),
                decoded);
            if (error != std::errc{} ||
                end != value.encoded_value.data() +
                           value.encoded_value.size()) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG input is not exact int64");
            }
            real_sum += static_cast<long double>(decoded);
            if (!std::isfinite(static_cast<double>(real_sum)) ||
                non_null_count == std::numeric_limits<std::uint64_t>::max()) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG state overflowed");
            }
            ++non_null_count;
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition AVG(DISTINCT) work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition AVG work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue average;
          average.descriptor = prepared.result_column.descriptor;
          average.is_null = non_null_count == 0;
          average.state = non_null_count == 0
                              ? api::EngineValueState::sql_null
                              : api::EngineValueState::value;
          if (non_null_count != 0) {
            const auto result =
                real_sum / static_cast<long double>(non_null_count);
            if (!std::isfinite(static_cast<double>(result))) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition AVG result overflowed");
            }
            std::ostringstream encoded;
            encoded << std::setprecision(17) << static_cast<double>(result);
            average.encoded_value = encoded.str();
          }
          tuple.values.push_back(std::move(average));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "AVG planning state: " + canonical.detail
                    : "AVG planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-avg-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (global_aggregate_profile.matched &&
            global_aggregate_profile.function ==
                exec::CanonicalAggregateFunction::sum) {
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state,
              exec::CanonicalAggregateFunction::sum, false,
              global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          std::int64_t sum_value = 0;
          bool has_value = false;
          const auto value_column = prepared.value_columns.front();
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (value.descriptor.canonical_type_name != "int64") {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition SUM input is not canonical int64");
            }
            if (prepared.distinct) {
              std::string key = value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            std::int64_t decoded = 0;
            const auto [end, error] = std::from_chars(
                value.encoded_value.data(),
                value.encoded_value.data() + value.encoded_value.size(),
                decoded);
            if (error != std::errc{} ||
                end != value.encoded_value.data() +
                           value.encoded_value.size() ||
                (decoded > 0 &&
                 sum_value >
                     std::numeric_limits<std::int64_t>::max() - decoded) ||
                (decoded < 0 &&
                 sum_value <
                     std::numeric_limits<std::int64_t>::min() - decoded)) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition SUM input or result overflowed");
            }
            sum_value += decoded;
            has_value = true;
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition SUM(DISTINCT) work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition SUM work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue sum;
          sum.descriptor = prepared.result_column.descriptor;
          sum.encoded_value = has_value ? std::to_string(sum_value) : "";
          sum.is_null = !has_value;
          sum.state = has_value ? api::EngineValueState::value
                                : api::EngineValueState::sql_null;
          tuple.values.push_back(std::move(sum));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "SUM planning state: " + canonical.detail
                    : "SUM planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              "canonical.aggregate.composed-global-sum-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::min ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::max)) {
          const auto function = global_aggregate_profile.function;
          const bool minimum =
              function == exec::CanonicalAggregateFunction::min;
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1 ||
              prepared.value_descriptor_ids.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          const auto value_column = prepared.value_columns.front();
          exec::CanonicalDescriptorOrderTerm comparison_term;
          comparison_term.column = value_column;
          comparison_term.expression_descriptor_id =
              prepared.value_descriptor_ids.front();
          const auto order_validation =
              exec::ValidateCanonicalDescriptorOrderTerm(
                  comparison_term, input_batch.columns[value_column]);
          if (!order_validation.ok) {
            return refuse(std::string(kPayloadDiagnostic),
                          "composition extremum comparison: " +
                              order_validation.detail);
          }
          std::set<std::string> distinct_values;
          std::optional<api::EngineTypedValue> extremum;
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (value.descriptor.canonical_type_name != "int64") {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition extremum input is not canonical "
                            "int64");
            }
            if (prepared.distinct) {
              std::string key = value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            if (!extremum.has_value()) {
              extremum = value;
              continue;
            }
            const auto compared =
                exec::CompareCanonicalDescriptorOrderValues(
                    value, *extremum, comparison_term);
            if (!compared.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition extremum comparison: " +
                                compared.diagnostic.detail);
            }
            if ((minimum && compared.comparison < 0) ||
                (!minimum && compared.comparison > 0)) {
              extremum = value;
            }
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition extremum DISTINCT work bound overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition extremum work exceeds the admitted bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue value;
          if (extremum.has_value()) value = std::move(*extremum);
          value.descriptor = prepared.result_column.descriptor;
          if (!extremum.has_value()) {
            value.encoded_value.clear();
            value.binary_value.clear();
            value.is_null = true;
            value.state = api::EngineValueState::sql_null;
          }
          tuple.values.push_back(std::move(value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "extremum planning state: " + canonical.detail
                    : "extremum planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              minimum
                  ? "canonical.aggregate.composed-global-min-expression.v1"
                  : "canonical.aggregate.composed-global-max-expression.v1";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (global_aggregate_profile.matched &&
            (global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_and ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::bool_or ||
             global_aggregate_profile.function ==
                 exec::CanonicalAggregateFunction::every)) {
          const auto function = global_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state, function,
              false, global_aggregate_profile.distinct,
              global_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 1) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          bool saw_value = false;
          bool saw_true = false;
          bool saw_false = false;
          const auto value_column = prepared.value_columns.front();
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& value = input_batch.rows[row].values[value_column];
            if (value.state == api::EngineValueState::sql_null ||
                value.is_null) {
              continue;
            }
            if (value.descriptor.canonical_type_name != "boolean") {
              return refuse(std::string(kPayloadDiagnostic),
                            "composition boolean aggregate input is not "
                            "canonical boolean");
            }
            if (prepared.distinct) {
              std::string key = value.encoded_value + ":";
              key.append(
                  reinterpret_cast<const char*>(value.binary_value.data()),
                  value.binary_value.size());
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            api::EngineSqlTruthValue truth =
                api::EngineSqlTruthValue::unspecified;
            std::string truth_detail;
            if (!api::QowCanonicalTruthFromTypedValueV1(
                    value, &truth, &truth_detail) ||
                (truth != api::EngineSqlTruthValue::true_value &&
                 truth != api::EngineSqlTruthValue::false_value)) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition boolean aggregate input: " + truth_detail);
            }
            saw_value = true;
            saw_true = saw_true ||
                       truth == api::EngineSqlTruthValue::true_value;
            saw_false = saw_false ||
                        truth == api::EngineSqlTruthValue::false_value;
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition boolean aggregate DISTINCT work bound "
                "overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition boolean aggregate work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue value;
          value.descriptor = prepared.result_column.descriptor;
          value.is_null = !saw_value;
          value.state = saw_value ? api::EngineValueState::value
                                  : api::EngineValueState::sql_null;
          if (saw_value) {
            const bool aggregate_truth =
                function == exec::CanonicalAggregateFunction::bool_or
                    ? saw_true
                    : !saw_false;
            value.encoded_value = aggregate_truth ? "true" : "false";
          }
          tuple.values.push_back(std::move(value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "boolean aggregate planning state: " +
                          canonical.detail
                    : "boolean aggregate planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          if (function == exec::CanonicalAggregateFunction::bool_and) {
            transformation_rule =
                "canonical.aggregate.composed-global-bool-and-expression.v1";
          } else if (function == exec::CanonicalAggregateFunction::bool_or) {
            transformation_rule =
                "canonical.aggregate.composed-global-bool-or-expression.v1";
          } else {
            transformation_rule =
                "canonical.aggregate.composed-global-every-expression.v1";
          }
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (pair_aggregate_profile.matched) {
          const auto function = pair_aggregate_profile.function;
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state, function,
              false, pair_aggregate_profile.distinct,
              pair_aggregate_profile.has_filter);
          if (!prepared.ok || prepared.value_columns.size() != 2) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::optional<std::vector<api::EngineSqlTruthValue>>
              filter_truth_values;
          if (prepared.filter_column.has_value()) {
            std::vector<api::EngineSqlTruthValue> materialized_filter;
            std::string filter_detail;
            if (!MaterializeAggregateFilterTruthValues(
                    input_batch, *prepared.filter_column,
                    prepared.filter_descriptor_id, &materialized_filter,
                    &filter_detail)) {
              return refuse(std::string(kPayloadDiagnostic),
                            std::move(filter_detail));
            }
            filter_truth_values = std::move(materialized_filter);
          }
          std::set<std::string> distinct_values;
          std::uint64_t non_null_count = 0;
          long double mean_x = 0.0L;
          long double mean_y = 0.0L;
          long double m2_x = 0.0L;
          long double m2_y = 0.0L;
          long double comoment = 0.0L;
          const auto y_column = prepared.value_columns[0];
          const auto x_column = prepared.value_columns[1];
          for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
            if (filter_truth_values.has_value() &&
                (*filter_truth_values)[row] !=
                    api::EngineSqlTruthValue::true_value) {
              continue;
            }
            const auto& y_value = input_batch.rows[row].values[y_column];
            const auto& x_value = input_batch.rows[row].values[x_column];
            if (y_value.state == api::EngineValueState::sql_null ||
                y_value.is_null ||
                x_value.state == api::EngineValueState::sql_null ||
                x_value.is_null) {
              continue;
            }
            if (y_value.descriptor.canonical_type_name != "int64" ||
                x_value.descriptor.canonical_type_name != "int64") {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical input is not canonical "
                  "int64");
            }
            if (prepared.distinct) {
              std::string key;
              for (const auto* value : {&y_value, &x_value}) {
                key.append(std::to_string(value->encoded_value.size()));
                key.push_back(':');
                key.append(value->encoded_value);
                key.push_back(':');
                key.append(reinterpret_cast<const char*>(
                               value->binary_value.data()),
                           value->binary_value.size());
                key.push_back('|');
              }
              if (!distinct_values.insert(std::move(key)).second) continue;
            }
            const auto decode = [](const api::EngineTypedValue& value,
                                   std::int64_t* decoded) {
              const auto [end, error] = std::from_chars(
                  value.encoded_value.data(),
                  value.encoded_value.data() + value.encoded_value.size(),
                  *decoded);
              return error == std::errc{} &&
                     end == value.encoded_value.data() +
                                value.encoded_value.size();
            };
            std::int64_t y_decoded = 0;
            std::int64_t x_decoded = 0;
            if (!decode(y_value, &y_decoded) ||
                !decode(x_value, &x_decoded) ||
                non_null_count == std::numeric_limits<std::uint64_t>::max()) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical input or count is invalid");
            }
            ++non_null_count;
            const auto y = static_cast<long double>(y_decoded);
            const auto x = static_cast<long double>(x_decoded);
            const auto count = static_cast<long double>(non_null_count);
            const auto delta_x = x - mean_x;
            mean_x += delta_x / count;
            const auto delta_y = y - mean_y;
            mean_y += delta_y / count;
            m2_x += delta_x * (x - mean_x);
            m2_y += delta_y * (y - mean_y);
            comoment += delta_x * (y - mean_y);
            if (!std::isfinite(static_cast<double>(mean_x)) ||
                !std::isfinite(static_cast<double>(mean_y)) ||
                !std::isfinite(static_cast<double>(m2_x)) ||
                !std::isfinite(static_cast<double>(m2_y)) ||
                !std::isfinite(static_cast<double>(comoment))) {
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical state overflowed");
            }
          }
          std::uint64_t aggregate_work = input_row_count;
          if (prepared.distinct &&
              !CheckedMultiply(input_row_count, input_row_count,
                               &aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition pair statistical DISTINCT work bound "
                "overflowed");
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition pair statistical work exceeds the admitted "
                "bound");
          }
          bool result_available = non_null_count != 0;
          long double statistic = 0.0L;
          switch (function) {
            case exec::CanonicalAggregateFunction::corr:
              result_available =
                  non_null_count >= 2 && m2_x > 0.0L && m2_y > 0.0L;
              if (result_available) {
                statistic = comoment / std::sqrt(m2_x * m2_y);
              }
              break;
            case exec::CanonicalAggregateFunction::covar_pop:
              if (result_available) {
                statistic =
                    comoment / static_cast<long double>(non_null_count);
              }
              break;
            case exec::CanonicalAggregateFunction::covar_samp:
              result_available = non_null_count >= 2;
              if (result_available) {
                statistic = comoment /
                            static_cast<long double>(non_null_count - 1);
              }
              break;
            case exec::CanonicalAggregateFunction::regr_avgx:
              statistic = mean_x;
              break;
            case exec::CanonicalAggregateFunction::regr_avgy:
              statistic = mean_y;
              break;
            case exec::CanonicalAggregateFunction::regr_intercept:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) {
                statistic = mean_y - mean_x * comoment / m2_x;
              }
              break;
            case exec::CanonicalAggregateFunction::regr_r2:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) {
                statistic = m2_y == 0.0L
                                ? 1.0L
                                : comoment * comoment / (m2_x * m2_y);
              }
              break;
            case exec::CanonicalAggregateFunction::regr_slope:
              result_available = result_available && m2_x != 0.0L;
              if (result_available) statistic = comoment / m2_x;
              break;
            case exec::CanonicalAggregateFunction::regr_sxx:
              statistic = m2_x;
              break;
            case exec::CanonicalAggregateFunction::regr_sxy:
              statistic = comoment;
              break;
            case exec::CanonicalAggregateFunction::regr_syy:
              statistic = m2_y;
              break;
            case exec::CanonicalAggregateFunction::regr_count:
              result_available = true;
              break;
            default:
              return refuse(
                  std::string(kPayloadDiagnostic),
                  "composition pair statistical function is unresolved");
          }
          if (result_available &&
              function != exec::CanonicalAggregateFunction::regr_count &&
              !std::isfinite(static_cast<double>(statistic))) {
            return refuse(
                std::string(kPayloadDiagnostic),
                "composition pair statistical result overflowed");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue result_value;
          result_value.descriptor = prepared.result_column.descriptor;
          result_value.is_null = !result_available;
          result_value.state = result_available
                                   ? api::EngineValueState::value
                                   : api::EngineValueState::sql_null;
          if (result_available) {
            if (function == exec::CanonicalAggregateFunction::regr_count) {
              if (non_null_count > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
                return refuse(
                    std::string(kPayloadDiagnostic),
                    "composition REGR_COUNT exceeded int64 result width");
              }
              result_value.encoded_value = std::to_string(non_null_count);
            } else {
              std::ostringstream encoded;
              encoded << std::setprecision(17)
                      << static_cast<double>(statistic);
              result_value.encoded_value = encoded.str();
            }
          }
          tuple.values.push_back(std::move(result_value));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "pair statistical planning state: " +
                          canonical.detail
                    : "pair statistical planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule =
              pair_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        if (string_aggregate_profile.matched ||
            ordered_collection_profile.matched ||
            json_object_profile.matched || listagg_profile.matched ||
            ordered_set_profile.matched || approximate_profile.matched) {
          exec::CanonicalAggregateFunction function =
              exec::CanonicalAggregateFunction::unknown;
          bool distinct = false;
          bool has_filter = false;
          bool comparison_sensitive = false;
          std::string transformation_id;
          if (string_aggregate_profile.matched) {
            function = exec::CanonicalAggregateFunction::string_agg;
            distinct = string_aggregate_profile.distinct;
            has_filter = string_aggregate_profile.has_filter;
            comparison_sensitive = string_aggregate_profile.ordered;
            transformation_id =
                string_aggregate_profile.transformation_id;
          } else if (ordered_collection_profile.matched) {
            function = ordered_collection_profile.function;
            distinct = ordered_collection_profile.distinct;
            has_filter = ordered_collection_profile.has_filter;
            comparison_sensitive = true;
            transformation_id =
                ordered_collection_profile.transformation_id;
          } else if (json_object_profile.matched) {
            function = exec::CanonicalAggregateFunction::json_object_agg;
            distinct = json_object_profile.distinct;
            has_filter = json_object_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = json_object_profile.transformation_id;
          } else if (listagg_profile.matched) {
            function = exec::CanonicalAggregateFunction::listagg;
            distinct = listagg_profile.distinct;
            has_filter = listagg_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = listagg_profile.transformation_id;
          } else if (ordered_set_profile.matched) {
            function = ordered_set_profile.function;
            distinct = ordered_set_profile.distinct;
            has_filter = ordered_set_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = ordered_set_profile.transformation_id;
          } else {
            function = approximate_profile.function;
            distinct = approximate_profile.distinct;
            has_filter = approximate_profile.has_filter;
            comparison_sensitive = true;
            transformation_id = approximate_profile.transformation_id;
          }
          auto prepared = PrepareGlobalAggregateRoot(
              request.relational_dag, node, input_node, state,
              function, false, distinct, has_filter);
          if (!prepared.ok) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          std::uint64_t aggregate_work = input_row_count;
          if (comparison_sensitive || prepared.distinct ||
              !prepared.aggregate_order_terms.empty()) {
            std::uint64_t comparison_work = 0;
            if (!CheckedMultiply(input_row_count, input_row_count,
                                 &comparison_work) ||
                !CheckedMultiply(
                    comparison_work,
                    std::max<std::size_t>(1,
                        prepared.value_columns.size()),
                    &comparison_work) ||
                !CheckedAdd(aggregate_work, comparison_work,
                            &aggregate_work)) {
              return refuse(
                  "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition complex aggregate work overflowed");
            }
          }
          if (!add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition complex aggregate work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns.push_back(prepared.result_column);
          exec::DescriptorTuple tuple;
          api::EngineTypedValue placeholder;
          placeholder.descriptor = prepared.result_column.descriptor;
          if (prepared.result_column.nullable) {
            placeholder.is_null = true;
            placeholder.state = api::EngineValueState::sql_null;
          } else if (
              placeholder.descriptor.canonical_type_name == "int64" ||
              placeholder.descriptor.canonical_type_name == "real64") {
            placeholder.encoded_value = "0";
            placeholder.state = api::EngineValueState::value;
          } else {
            return refuse(
                std::string(kPayloadDiagnostic),
                "complex aggregate non-null planning descriptor is not "
                "cardinality-only safe");
          }
          tuple.values.push_back(std::move(placeholder));
          output.rows.push_back(std::move(tuple));
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          // Cardinality-only planning carries a canonical SQL NULL for the
          // derived ARRAY_AGG list descriptor; scalar value validation does
          // not own derived collection encodings.
          const bool derived_list_planning_descriptor =
              output.columns.front().descriptor.canonical_type_name.rfind(
                  "list<", 0) == 0;
          const auto values = derived_list_planning_descriptor
                                  ? exec::DescriptorRuntimeDiagnostic{}
                                  : exec::ValidateDescriptorBatch(output);
          if (!canonical.ok ||
              (!derived_list_planning_descriptor && !values.ok)) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "complex aggregate planning state: " +
                          canonical.detail
                    : "complex aggregate planning state: " + values.detail);
          }
          prepared_registry_aggregate = std::move(prepared);
          registry_aggregate_input_row_count = input_row_count;
          state.batch = std::move(output);
          state.result_bindings =
              prepared_registry_aggregate->result_bindings;
          implementation_id = "aggregate.registry-core.v1";
          capability_uuid = registry_aggregate_capability_uuid;
          transformation_rule = transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          planning_values_exact = false;
          break;
        }
        if (grouped_aggregate_profile.matched) {
          auto prepared = PrepareGroupedCountSumRoot(
              request.relational_dag, node, input_node, state,
              grouped_aggregate_profile);
          const auto expected_projection_count =
              grouped_aggregate_profile.projects_grouping_metadata
                  ? grouped_aggregate_profile.key_count + 1
                  : 0;
          if (!prepared.ok ||
              prepared.key_terms.size() !=
                  grouped_aggregate_profile.key_count ||
              prepared.key_result_columns.size() !=
                  grouped_aggregate_profile.key_count ||
              prepared.grouping_sets.empty() ||
              prepared.count.value_columns.size() != 0 ||
              prepared.sum.value_columns.size() != 1 ||
              prepared.grouping_projection_columns.size() !=
                  expected_projection_count) {
            return refuse(std::string(kPayloadDiagnostic), prepared.detail);
          }
          struct GroupPlanningState {
            std::size_t grouping_set_ordinal{0};
            std::vector<bool> grouping_indicators;
            std::uint64_t grouping_id{0};
            std::size_t representative_row{0};
            bool has_representative{false};
            std::uint64_t count{0};
            __int128 sum{0};
            bool has_sum{false};
          };
          std::vector<GroupPlanningState> groups;
          std::uint64_t output_row_bound = 0;
          if (!CheckedMultiply(
                  std::max<std::uint64_t>(1, input_row_count),
                  prepared.grouping_sets.size(), &output_row_bound) ||
              output_row_bound >
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::size_t>::max())) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                "composition grouped aggregate output bound overflowed");
          }
          groups.reserve(static_cast<std::size_t>(output_row_bound));
          const auto sum_column = prepared.sum.value_columns.front();
          for (std::size_t set_ordinal = 0;
               set_ordinal < prepared.grouping_sets.size(); ++set_ordinal) {
            const auto& grouping_set = prepared.grouping_sets[set_ordinal];
            std::vector<bool> included(prepared.key_terms.size(), false);
            for (const auto key_ordinal :
                 grouping_set.key_term_ordinals) {
              included[key_ordinal] = true;
            }
            const auto metadata =
                exec::ComputeCanonicalAggregateGroupingMetadata(
                    prepared.key_terms.size(), grouping_set);
            if (!metadata.diagnostic.ok) {
              return refuse(std::string(kPayloadDiagnostic),
                            "composed grouped metadata: " +
                                metadata.diagnostic.detail);
            }
            const auto set_begin = groups.size();
            if (std::ranges::find(included, true) == included.end()) {
              GroupPlanningState grand_total;
              grand_total.grouping_set_ordinal = set_ordinal;
              grand_total.grouping_indicators =
                  metadata.grouping_indicators;
              grand_total.grouping_id = metadata.grouping_id;
              groups.push_back(std::move(grand_total));
            }
            for (std::size_t row_ordinal = 0;
                 row_ordinal < input_batch.rows.size(); ++row_ordinal) {
              const auto& row = input_batch.rows[row_ordinal];
              if (sum_column >= row.values.size() ||
                  std::ranges::any_of(
                      prepared.key_terms, [&](const auto& term) {
                        return term.column >= row.values.size();
                      })) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped row shape is incomplete");
              }
              auto group = groups.end();
              for (auto candidate =
                       groups.begin() +
                           static_cast<std::ptrdiff_t>(set_begin);
                   candidate != groups.end(); ++candidate) {
                bool matches = true;
                for (std::size_t key_ordinal = 0;
                     key_ordinal < prepared.key_terms.size();
                     ++key_ordinal) {
                  if (!included[key_ordinal]) continue;
                  const auto& key_term = prepared.key_terms[key_ordinal];
                  const auto compared =
                      exec::CompareCanonicalDescriptorOrderValues(
                          row.values[key_term.column],
                          input_batch.rows[candidate->representative_row]
                              .values[key_term.column],
                          key_term);
                  if (!compared.diagnostic.ok) {
                    return refuse(
                        std::string(kPayloadDiagnostic),
                        "composed grouped key comparison: " +
                            compared.diagnostic.detail);
                  }
                  if (compared.comparison != 0) {
                    matches = false;
                    break;
                  }
                }
                if (matches) {
                  group = candidate;
                  break;
                }
              }
              if (group == groups.end()) {
                GroupPlanningState created;
                created.grouping_set_ordinal = set_ordinal;
                created.grouping_indicators =
                    metadata.grouping_indicators;
                created.grouping_id = metadata.grouping_id;
                created.representative_row = row_ordinal;
                created.has_representative = true;
                groups.push_back(std::move(created));
                group = std::prev(groups.end());
              } else if (!group->has_representative) {
                group->representative_row = row_ordinal;
                group->has_representative = true;
              }
              if (group->count ==
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped COUNT(*) overflowed");
              }
              ++group->count;
              const auto& amount = row.values[sum_column];
              if (amount.state == api::EngineValueState::sql_null ||
                  amount.is_null) {
                continue;
              }
              if (amount.descriptor.canonical_type_name != "int64") {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM input is not int64");
              }
              std::int64_t decoded = 0;
              const auto [end, error] = std::from_chars(
                  amount.encoded_value.data(),
                  amount.encoded_value.data() +
                      amount.encoded_value.size(),
                  decoded);
              if (error != std::errc{} ||
                  end != amount.encoded_value.data() +
                             amount.encoded_value.size()) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM input is invalid");
              }
              group->sum += static_cast<__int128>(decoded);
              group->has_sum = true;
            }
          }
          std::uint64_t input_pairs = 0;
          std::uint64_t comparison_work = 0;
          std::uint64_t transition_work = 0;
          std::uint64_t aggregate_work = 0;
          if (!CheckedMultiply(input_row_count, input_row_count,
                               &input_pairs) ||
              !CheckedMultiply(input_pairs, prepared.key_terms.size(),
                               &comparison_work) ||
              !CheckedMultiply(comparison_work,
                               prepared.grouping_sets.size(),
                               &comparison_work) ||
              !CheckedMultiply(input_row_count,
                               prepared.grouping_sets.size(),
                               &transition_work) ||
              !CheckedAdd(comparison_work, transition_work,
                          &aggregate_work) ||
              !add_work(aggregate_work)) {
            return refuse(
                "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                "composition grouped aggregate work exceeds the admitted "
                "bound");
          }
          exec::DescriptorBatch output;
          output.columns = prepared.key_result_columns;
          output.columns.push_back(prepared.count.result_column);
          output.columns.push_back(prepared.sum.result_column);
          output.columns.insert(
              output.columns.end(),
              prepared.grouping_projection_columns.begin(),
              prepared.grouping_projection_columns.end());
          output.rows.reserve(groups.size());
          for (auto& group : groups) {
            exec::DescriptorTuple tuple;
            const auto& grouping_set =
                prepared.grouping_sets[group.grouping_set_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue key;
              key.descriptor =
                  prepared.key_result_columns[key_ordinal].descriptor;
              const bool included =
                  std::ranges::find(grouping_set.key_term_ordinals,
                                    key_ordinal) !=
                  grouping_set.key_term_ordinals.end();
              if (included) {
                if (!group.has_representative) {
                  return refuse(std::string(kPayloadDiagnostic),
                                "composed grouped key has no representative");
                }
                key = input_batch.rows[group.representative_row]
                          .values[prepared.key_terms[key_ordinal].column];
                key.descriptor =
                    prepared.key_result_columns[key_ordinal].descriptor;
              } else {
                key.is_null = true;
                key.state = api::EngineValueState::sql_null;
              }
              tuple.values.push_back(std::move(key));
            }
            api::EngineTypedValue count;
            count.descriptor = prepared.count.result_column.descriptor;
            count.encoded_value = std::to_string(group.count);
            count.state = api::EngineValueState::value;
            tuple.values.push_back(std::move(count));
            api::EngineTypedValue sum;
            sum.descriptor = prepared.sum.result_column.descriptor;
            sum.is_null = !group.has_sum;
            sum.state = group.has_sum ? api::EngineValueState::value
                                      : api::EngineValueState::sql_null;
            if (group.has_sum) {
              if (group.sum < static_cast<__int128>(
                                  std::numeric_limits<std::int64_t>::min()) ||
                  group.sum > static_cast<__int128>(
                                  std::numeric_limits<std::int64_t>::max())) {
                return refuse(std::string(kPayloadDiagnostic),
                              "composed grouped SUM result overflowed");
              }
              sum.encoded_value = std::to_string(
                  static_cast<std::int64_t>(group.sum));
            }
            tuple.values.push_back(std::move(sum));
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size() &&
                 !prepared.grouping_projection_columns.empty();
                 ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  group.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              tuple.values.push_back(std::move(indicator));
            }
            if (!prepared.grouping_projection_columns.empty()) {
              api::EngineTypedValue grouping_id;
              grouping_id.descriptor =
                  prepared.grouping_projection_columns.back().descriptor;
              grouping_id.encoded_value =
                  std::to_string(group.grouping_id);
              grouping_id.state = api::EngineValueState::value;
              tuple.values.push_back(std::move(grouping_id));
            }
            output.rows.push_back(std::move(tuple));
          }
          const auto canonical = exec::ValidateCanonicalDescriptorBatch(
              output, node.output_descriptor_ids);
          const auto values = exec::ValidateDescriptorBatch(output);
          if (!canonical.ok || !values.ok) {
            return refuse(
                std::string(kPayloadDiagnostic),
                !canonical.ok
                    ? "grouped planning state: " + canonical.detail
                    : "grouped planning state: " + values.detail);
          }
          grouped_aggregate_input_row_count = input_row_count;
          grouped_aggregate_output_row_bound =
              static_cast<std::size_t>(output_row_bound);
          prepared_grouped_aggregate = std::move(prepared);
          state.batch = std::move(output);
          state.result_bindings =
              prepared_grouped_aggregate->result_bindings;
          implementation_id = "aggregate.registry-grouping-sets.v1";
          capability_uuid = grouped_aggregate_capability_uuid;
          transformation_rule =
              grouped_aggregate_profile.transformation_id + ".composed";
          physical_kind = exec::PhysicalNodeKind::kAggregate;
          auxiliary_memory = aggregate_work;
          break;
        }
        auto prepared = PrepareQueryDistinctRoot(
            request.context, request.relational_dag, node, input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::uint64_t pair_count = 0;
        std::uint64_t pair_value_count = 0;
        std::uint64_t self_value_count = 0;
        std::uint64_t comparison_bound = 0;
        if (!CheckedMultiply(input_row_count, input_row_count, &pair_count) ||
            !CheckedMultiply(pair_count, input_batch.columns.size(),
                             &pair_value_count) ||
            !CheckedMultiply(input_row_count, input_batch.columns.size(),
                             &self_value_count) ||
            !CheckedAdd(pair_value_count, self_value_count,
                        &comparison_bound) ||
            comparison_bound > std::numeric_limits<std::size_t>::max() ||
            !add_work(comparison_bound)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition DISTINCT comparison bound overflowed or was exhausted");
        }
        std::vector<std::size_t> representatives;
        representatives.reserve(input_row_count);
        for (std::size_t row = 0; row < input_row_count; ++row) {
          bool duplicate = false;
          for (const auto representative : representatives) {
            bool equal = true;
            for (const auto& term : prepared.equality_terms) {
              const auto compared = exec::CompareCanonicalDescriptorOrderValues(
                  input_batch.rows[row].values[term.column],
                  input_batch.rows[representative].values[term.column], term);
              if (!compared.diagnostic.ok) {
                return refuse(std::string(kPayloadDiagnostic),
                              "DISTINCT comparison: " +
                                  compared.diagnostic.detail);
              }
              if (compared.comparison != 0) {
                equal = false;
                break;
              }
            }
            if (equal) {
              duplicate = true;
              break;
            }
          }
          if (!duplicate) representatives.push_back(row);
        }
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(representatives.size());
        for (const auto row : representatives) {
          output.rows.push_back(input_batch.rows[row]);
        }
        prepared_distinct = std::move(prepared);
        distinct_input_row_count = input_row_count;
        distinct_comparison_bound =
            std::max<std::size_t>(1,
                static_cast<std::size_t>(comparison_bound));
        state.batch = std::move(output);
        state.result_bindings = prepared_distinct->result_bindings;
        implementation_id = "aggregate.query-distinct.typed.v1";
        capability_uuid = distinct_capability_uuid;
        transformation_rule =
            "canonical.aggregate.composed-query-distinct.v1";
        physical_kind = exec::PhysicalNodeKind::kAggregate;
        auxiliary_memory = comparison_bound;
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kSort: {
        const bool expression_ordering = std::ranges::any_of(
            node.bound_expression_ids, [&](const auto expression_id) {
              return std::ranges::find(input_node.bound_expression_ids,
                                       expression_id) ==
                     input_node.bound_expression_ids.end();
            });
        auto prepared =
            expression_ordering
                ? PrepareExpressionSortRoot(
                      request.context, request.relational_dag,
                      request.optimizer_request.logical_properties, node,
                      input_node, state, request.expression_services)
                : PrepareSortRoot(
                      request.context, request.relational_dag,
                      request.optimizer_request.logical_properties, node,
                      input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        std::uint64_t expression_work = 0;
        std::uint64_t expression_memory = 0;
        if (prepared.expression_ordering &&
            (!CheckedMultiply(input_row_count, prepared.expressions.size(),
                              &expression_work) ||
             !AddBatchMemoryBytes(prepared.expression_input_batch,
                                  &expression_memory) ||
             !add_work(expression_work))) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition expression SORT evaluation bound overflowed or "
              "was exhausted");
        }
        std::uint64_t comparison_bound = 0;
        std::uint64_t row_order_memory = 0;
        if (!CheckedMultiply(input_row_count, input_row_count,
                             &comparison_bound) ||
            !CheckedMultiply(input_row_count, sizeof(std::size_t),
                             &row_order_memory) ||
            comparison_bound > std::numeric_limits<std::size_t>::max() ||
            !CheckedAdd(comparison_bound, row_order_memory,
                        &auxiliary_memory) ||
            (prepared.expression_ordering &&
             (!CheckedAdd(auxiliary_memory, expression_memory,
                          &auxiliary_memory) ||
              !CheckedAdd(auxiliary_memory, expression_memory,
                          &auxiliary_memory))) ||
            !add_work(comparison_bound)) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition SORT comparison bound overflowed or was exhausted");
        }
        const auto& order_key_batch = prepared.expression_ordering
                                          ? prepared.expression_input_batch
                                          : input_batch;
        std::vector<std::int8_t> comparisons(
            static_cast<std::size_t>(comparison_bound), 0);
        for (std::size_t left = 0; left < input_row_count; ++left) {
          for (std::size_t right = left + 1; right < input_row_count;
               ++right) {
            int comparison = 0;
            for (const auto& term : prepared.order_terms) {
              const auto compared = exec::CompareCanonicalDescriptorOrderValues(
                  order_key_batch.rows[left].values[term.column],
                  order_key_batch.rows[right].values[term.column], term);
              if (!compared.diagnostic.ok) {
                return refuse(std::string(kPayloadDiagnostic),
                              "SORT comparison: " +
                                  compared.diagnostic.detail);
              }
              comparison = compared.comparison;
              if (comparison != 0) break;
            }
            comparisons[left * input_row_count + right] =
                static_cast<std::int8_t>(comparison);
            comparisons[right * input_row_count + left] =
                static_cast<std::int8_t>(-comparison);
          }
        }
        std::vector<std::size_t> row_order(input_row_count);
        std::iota(row_order.begin(), row_order.end(), 0);
        std::stable_sort(row_order.begin(), row_order.end(),
                         [&](const auto left, const auto right) {
                           return comparisons[left * input_row_count + right] <
                                  0;
                         });
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(input_row_count);
        for (const auto row : row_order) {
          output.rows.push_back(input_batch.rows[row]);
        }
        prepared_sort = std::move(prepared);
        sort_input_row_count = input_row_count;
        sort_comparison_bound = std::max<std::size_t>(
            1, static_cast<std::size_t>(comparison_bound));
        state.batch = std::move(output);
        state.result_bindings = prepared_sort->result_bindings;
        implementation_id = prepared_sort->expression_ordering
                                ? "sort.typed.expression-row.v1"
                                : "sort.typed.terms.v1";
        capability_uuid = sort_capability_uuid;
        transformation_rule = prepared_sort->expression_ordering
                                  ? "canonical.sort.composed-expression-row.v1"
                                  : "canonical.sort.composed-typed-terms.v1";
        physical_kind = exec::PhysicalNodeKind::kSort;
        delivered_property_uuids = {prepared_sort->ordering_property_uuid};
        property_kinds = {
            plan::CanonicalLogicalPropertyKind::kOrdering};
        break;
      }
      case plan::CanonicalLogicalRelationalNodeKind::kLimit: {
        auto prepared = PrepareLimitRoot(request.relational_dag, node,
                                         input_node, state);
        if (!prepared.ok) {
          return refuse(std::string(kPayloadDiagnostic), prepared.detail);
        }
        fetch_first_rows_only =
            node.semantic_variant_id ==
            "fetch.first-rows-only-offset.v1";
        const bool has_offset = node.bound_expression_ids.size() == 2;
        const auto expected_arity =
            node.semantic_variant_id == "limit.bound-count.v1" ? 1U : 2U;
        if (node.bound_expression_ids.size() != expected_arity) {
          return refuse(std::string(kPayloadDiagnostic),
                        "LIMIT/FETCH bound arity is not exact");
        }
        std::string detail;
        if (!EvaluateNonNegativeRowBound(
                &expression_runtime, node.bound_expression_ids.front(),
                &row_limit, &detail) ||
            (has_offset &&
             !EvaluateNonNegativeRowBound(
                 &expression_runtime, node.bound_expression_ids[1],
                 &row_offset, &detail))) {
          return refuse(std::string(kPayloadDiagnostic),
                        "LIMIT/FETCH bound: " + detail);
        }
        const auto offset = row_offset > input_row_count
                                ? input_row_count
                                : static_cast<std::size_t>(row_offset);
        const auto remaining = input_row_count - offset;
        const auto count = row_limit > remaining
                               ? remaining
                               : static_cast<std::size_t>(row_limit);
        exec::DescriptorBatch output;
        output.columns = input_batch.columns;
        output.rows.reserve(count);
        for (std::size_t row = 0; row < count; ++row) {
          output.rows.push_back(input_batch.rows[offset + row]);
        }
        if (!add_work(node.bound_expression_ids.size())) {
          return refuse(
              "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
              "composition LIMIT/FETCH work exceeds the admitted bound");
        }
        prepared_limit = std::move(prepared);
        limit_input_row_count = input_row_count;
        state.batch = std::move(output);
        state.result_bindings = prepared_limit->result_bindings;
        limit_implementation_id = fetch_first_rows_only
            ? "fetch.native.rows-only.v1"
            : "limit.typed.v1";
        implementation_id = limit_implementation_id;
        capability_uuid = limit_capability_uuid;
        transformation_rule = fetch_first_rows_only
            ? "canonical.fetch.composed-first-rows-only-offset.v1"
            : "canonical.limit.composed-bound-count-offset.v1";
        physical_kind = exec::PhysicalNodeKind::kLimit;
        break;
      }
      default:
        return refuse(std::string(kPayloadDiagnostic),
                      "composition node kind changed after shape admission");
    }

    std::uint64_t output_memory = 1;
    std::uint64_t operator_memory = 0;
    if (!AddBatchMemoryBytes(state.batch, &output_memory) ||
        !CheckedAdd(input_memory, output_memory, &operator_memory) ||
        !CheckedAdd(operator_memory, auxiliary_memory, &operator_memory) ||
        operator_memory >
            request.optimizer_request.resource.memory_budget_bytes) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                    "composition node exceeds the admitted memory budget");
    }
    profiles.push_back(
        {node.logical_node_id, implementation_id, capability_uuid,
         node.node_kind, physical_kind, transformation_rule,
         state.batch.rows.size(), operator_memory, 1, 1,
         std::move(required_property_uuids),
         std::move(delivered_property_uuids), std::move(property_kinds)});
  }

  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "node-composition.selected-plan",
      "node-driven composition");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  if (join_kind.has_value()) {
    values_batches.emplace(join_left_node->logical_node_id,
                           std::move(join_left_values->batch));
    values_batches.emplace(join_right_node->logical_node_id,
                           std::move(join_right_values->batch));
  } else if (!set_base_nodes.empty()) {
    for (auto& [node_id, materialized] : set_materialized_values) {
      const auto base_node = set_base_nodes.at(node_id);
      if (base_node->node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kValues) {
        values_batches.emplace(node_id, std::move(materialized.batch));
      }
    }
  } else {
    auto values = MaterializeValues(request.relational_dag,
                                    *reverse_chain.front(),
                                    request.expression_services);
    if (!values.ok) {
      return refuse(std::string(kPayloadDiagnostic),
                    "composition VALUES replay: " + values.detail);
    }
    values_batches.emplace(reverse_chain.front()->logical_node_id,
                           std::move(values.batch));
  }

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      MakeLiveValuesRegistration(
          std::move(values_batches), values_capability_uuid,
          "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-VALUES-V1",
          "node-driven composition"));
  if (prepared_join.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveJoinRegistration(
            join_implementation_id, join_capability_uuid,
            std::move(join_truth_values), join_pair_count,
            join_output_row_bound, *join_kind, join_operation_name,
            request.context));
  }
  std::unordered_set<std::string> registered_set_implementations;
  for (const auto& [node_id, prepared] : prepared_set_nodes) {
    (void)node_id;
    if (!registered_set_implementations
             .insert(prepared.profile.implementation_id)
             .second) {
      continue;
    }
    execution_request.available_executors.push_back(
        MakeLiveSetOperationRegistration(
            prepared_set_nodes, prepared.profile.implementation_id,
            set_capability_uuids.at(prepared.profile.implementation_id),
            request.context));
  }
  if (prepared_filter.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveFilterRegistration(
            std::move(filter_truth_values), filter_capability_uuid,
            filter_input_row_count, request.context));
  }
  if (prepared_project.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveProjectRegistration(
            *prepared_project, project_implementation_id,
            project_capability_uuid, project_input_row_count,
            request.relational_dag, request.expression_services,
            request.context));
  }
  if (prepared_distinct.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveQueryDistinctRegistration(
            std::move(prepared_distinct->equality_terms),
            distinct_capability_uuid, distinct_input_row_count,
            distinct_comparison_bound, request.context));
  }
  if (prepared_count_star.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveCountStarRegistration(
            prepared_count_star->result_column,
            count_star_capability_uuid, count_star_input_row_count,
            request.context));
  }
  if (prepared_registry_aggregate.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveAggregateRegistryRegistration(
            *prepared_registry_aggregate,
            registry_aggregate_capability_uuid,
            registry_aggregate_input_row_count, request.context));
  }
  if (prepared_grouped_aggregate.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveGroupedCountSumRegistration(
            *prepared_grouped_aggregate,
            grouped_aggregate_capability_uuid,
            grouped_aggregate_input_row_count,
            grouped_aggregate_output_row_bound, request.context));
  }
  if (prepared_sort.has_value()) {
    const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + prepared_sort->ordering_property_uuid,
        "node-composition.deterministic-tie");
    if (prepared_sort->expression_ordering) {
      execution_request.available_executors.push_back(
          MakeLiveExpressionSortRegistration(
              std::move(prepared_sort->order_terms),
              std::move(prepared_sort->expressions),
              deterministic_tie_evidence_uuid, sort_capability_uuid,
              sort_input_row_count, sort_comparison_bound,
              request.relational_dag, request.expression_services,
              request.context));
    } else {
      execution_request.available_executors.push_back(
          MakeLiveSortRegistration(
              std::move(prepared_sort->order_terms),
              deterministic_tie_evidence_uuid, sort_capability_uuid,
              sort_input_row_count, sort_comparison_bound,
              request.context));
    }
  }
  if (prepared_limit.has_value()) {
    execution_request.available_executors.push_back(
        MakeLiveLimitRegistration(
            limit_implementation_id, limit_capability_uuid, row_limit,
            row_offset, fetch_first_rows_only, limit_input_row_count,
            request.context));
  }

  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "node-composition.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "node-composition.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(state.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, state.batch.rows.size());

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-NODE-COMPOSITION-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "node-driven composition selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  const auto set_profile =
      root == graph.nodes.end()
          ? LiveSetOperationProfile{}
          : MatchLiveSetOperationProfile(root->semantic_variant_id);
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      !set_profile.matched ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      !root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "live set-operation execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareSetOperationRoot(
      request.context, request.relational_dag, *root, left, right,
      set_profile);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                  prepared_root.detail);
  }

  if (right.batch.rows.size() >
      std::numeric_limits<std::size_t>::max() - left.batch.rows.size()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation row bound overflowed");
  }
  const auto set_output_row_bound =
      left.batch.rows.size() + right.batch.rows.size();
  std::uint64_t set_comparison_bound = 0;
  if (!CheckedMultiply(set_output_row_bound, set_output_row_bound,
                       &set_comparison_bound)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation comparison bound overflowed");
  }
  const bool concatenation_only =
      set_profile.operation == exec::CanonicalSetOperationKind::kUnion &&
      set_profile.quantifier == exec::CanonicalSetOperationQuantifier::kAll;
  const auto set_work =
      concatenation_only
          ? static_cast<std::uint64_t>(set_output_row_bound)
          : set_comparison_bound;
  if (set_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live set-operation work exceeds the admitted candidate "
                  "bound");
  }

  std::uint64_t memory_bytes = 1;
  if (!AddBatchMemoryBytes(left.batch, &memory_bytes) ||
      !AddBatchMemoryBytes(right.batch, &memory_bytes) ||
      !CheckedAdd(memory_bytes, set_work, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live set-operation materialization size overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live set-operation inputs exceed the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto set_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "set." + set_profile.identity_component +
                          ".capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = set_output_row_bound;
    std::uint64_t node_memory = memory_bytes;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(left.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "left VALUES cost size overflowed");
      }
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right.batch.rows.size();
      node_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &node_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "right VALUES cost size overflowed");
      }
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : set_profile.implementation_id,
         values ? values_capability_uuid : set_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kSetOperation,
         values ? "canonical.values.materialize.v1"
                : set_profile.physical_semantic_id,
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      "set." + set_profile.identity_component + ".selected-plan",
      set_profile.operation_name);
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
      set_profile.operation_name);

  exec::CanonicalPhysicalExecutorRegistration set_registration;
  set_registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
  set_registration.implementation_id = set_profile.implementation_id;
  set_registration.executor_capability_uuid = set_capability_uuid;
  set_registration.executor_capability_abi_version = 1;
  set_registration.engine_owned = true;
  set_registration.accepts_optimizer_publication_v2 = true;
  set_registration.execute =
      [result_columns = prepared_root.result_columns,
       collation_bindings = prepared_root.collation_bindings,
       set_profile, set_output_row_bound, set_comparison_bound,
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
          step.diagnostic.detail =
              "set-operation executor did not receive two typed input batches";
          return step;
        }
        exec::CanonicalSetOperationAllRequest set_request;
        set_request.physical_dag = dag;
        set_request.selected_physical_node_id = node.physical_node_id;
        set_request.left_batch = *inputs[0].materialized_output_batch;
        set_request.right_batch = *inputs[1].materialized_output_batch;
        set_request.result_columns = result_columns;
        set_request.operation = set_profile.operation;
        set_request.alignment = set_profile.alignment;
        set_request.quantifier = set_profile.quantifier;
        set_request.equality_profile = set_profile.equality_profile;
        set_request.type_profile = set_profile.type_profile;
        set_request.collation_bindings = collation_bindings;
        set_request.maximum_equality_comparison_count =
            std::max<std::size_t>(
                1, static_cast<std::size_t>(set_comparison_bound));
        set_request.maximum_output_row_count =
            std::max<std::size_t>(1, set_output_row_bound);
        set_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto set_result =
            set_profile.quantifier ==
                    exec::CanonicalSetOperationQuantifier::kAll
                ? exec::ExecuteCanonicalSetOperationAll(set_request)
                : exec::ExecuteCanonicalSetOperationDistinct(set_request);
        if (!set_result.diagnostic.ok) {
          step.diagnostic = set_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = set_result.left_input_row_count +
                               set_result.right_input_row_count;
        step.rows_examined = step.input_row_count;
        step.output_row_count = set_result.output_batch.rows.size();
        step.materialized_output_batch = set_result.output_batch;
        step.mga_statement_context = set_result.mga_statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(set_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "set." + set_profile.identity_component + ".execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "set." + set_profile.identity_component +
              ".transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, set_output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live set-operation selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeNestedSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() < 5 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }

  std::unordered_map<std::uint64_t, const plan::CanonicalLogicalRelationalNode*>
      nodes;
  std::unordered_map<std::uint64_t, LiveSetOperationProfile> set_profiles;
  std::size_t values_count = 0;
  for (const auto& node : graph.nodes) {
    if (!nodes.emplace(node.logical_node_id, &node).second ||
        !node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
    if (node.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      if (!node.input_logical_node_ids.empty() ||
          node.semantic_variant_id != "values.literal-table.v1") {
        return result;
      }
      ++values_count;
      continue;
    }
    if (node.node_kind !=
        plan::CanonicalLogicalRelationalNodeKind::kSetOperation) {
      return result;
    }
    auto profile = MatchLiveSetOperationProfile(node.semantic_variant_id);
    if (!profile.matched || node.input_logical_node_ids.size() != 2 ||
        node.input_logical_node_ids[0] == node.input_logical_node_ids[1] ||
        !node.bound_expression_ids.empty()) {
      return result;
    }
    set_profiles.emplace(node.logical_node_id, std::move(profile));
  }
  if (values_count < 3 || set_profiles.size() < 2 ||
      values_count + set_profiles.size() != graph.nodes.size()) {
    return result;
  }

  std::unordered_set<std::uint64_t> reachable;
  std::vector<std::uint64_t> pending_reachability{root->logical_node_id};
  while (!pending_reachability.empty()) {
    const auto node_id = pending_reachability.back();
    pending_reachability.pop_back();
    if (!reachable.insert(node_id).second) continue;
    const auto found = nodes.find(node_id);
    if (found == nodes.end()) return result;
    for (const auto input_id : found->second->input_logical_node_ids) {
      pending_reachability.push_back(input_id);
    }
  }
  if (reachable.size() != graph.nodes.size()) return result;

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-ADMISSION-V1",
                  "nested set-operation execution lacks optimizer admission");
  }

  std::unordered_map<std::uint64_t, MaterializedValues> materialized_schemas;
  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  std::unordered_map<std::uint64_t, std::uint64_t> row_bounds;
  std::unordered_map<std::uint64_t, std::uint64_t> leaf_memory;
  std::uint64_t memory_bytes = 1;
  for (const auto& node : graph.nodes) {
    if (node.node_kind !=
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      continue;
    }
    auto materialized = MaterializeValues(
        request.relational_dag, node, request.expression_services);
    if (!materialized.ok) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                    "nested VALUES: " + materialized.detail);
    }
    std::uint64_t node_memory = 1;
    if (!AddBatchMemoryBytes(materialized.batch, &node_memory) ||
        !AddBatchMemoryBytes(materialized.batch, &memory_bytes)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "nested set-operation leaf memory overflowed");
    }
    row_bounds.emplace(node.logical_node_id,
                       materialized.batch.rows.size());
    leaf_memory.emplace(node.logical_node_id, node_memory);
    values_batches.emplace(node.logical_node_id, materialized.batch);
    materialized_schemas.emplace(node.logical_node_id,
                                 std::move(materialized));
  }

  std::unordered_map<std::uint64_t, PreparedLiveSetNode> prepared_set_nodes;
  std::unordered_set<std::uint64_t> pending_set_nodes;
  for (const auto& [node_id, profile] : set_profiles) {
    (void)profile;
    pending_set_nodes.insert(node_id);
  }
  std::uint64_t total_set_work = 0;
  while (!pending_set_nodes.empty()) {
    bool progressed = false;
    for (auto pending = pending_set_nodes.begin();
         pending != pending_set_nodes.end();) {
      const auto node_id = *pending;
      const auto* node = nodes.at(node_id);
      const auto left = materialized_schemas.find(
          node->input_logical_node_ids[0]);
      const auto right = materialized_schemas.find(
          node->input_logical_node_ids[1]);
      if (left == materialized_schemas.end() ||
          right == materialized_schemas.end()) {
        ++pending;
        continue;
      }
      auto prepared = PrepareSetOperationRoot(
          request.context, request.relational_dag, *node, left->second,
          right->second, set_profiles.at(node_id));
      if (!prepared.ok) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                      prepared.detail);
      }
      std::uint64_t output_bound = 0;
      std::uint64_t comparison_bound = 0;
      const auto left_bound =
          row_bounds.at(node->input_logical_node_ids[0]);
      const auto right_bound =
          row_bounds.at(node->input_logical_node_ids[1]);
      if (!CheckedAdd(left_bound, right_bound, &output_bound) ||
          !CheckedMultiply(output_bound, output_bound, &comparison_bound)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "nested set-operation row/comparison bound overflowed");
      }
      const bool concatenation_only =
          set_profiles.at(node_id).operation ==
              exec::CanonicalSetOperationKind::kUnion &&
          set_profiles.at(node_id).quantifier ==
              exec::CanonicalSetOperationQuantifier::kAll;
      const auto node_work =
          concatenation_only ? output_bound : comparison_bound;
      if (!CheckedAdd(total_set_work, node_work, &total_set_work)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "nested set-operation work bound overflowed");
      }
      MaterializedValues schema;
      schema.ok = true;
      schema.batch.columns = prepared.result_columns;
      schema.result_bindings = prepared.result_bindings;
      row_bounds.emplace(node_id, output_bound);
      materialized_schemas.emplace(node_id, std::move(schema));
      prepared_set_nodes.emplace(
          node_id,
          PreparedLiveSetNode{
              set_profiles.at(node_id), std::move(prepared),
              static_cast<std::size_t>(output_bound),
              static_cast<std::size_t>(std::max<std::uint64_t>(
                  1, comparison_bound))});
      pending = pending_set_nodes.erase(pending);
      progressed = true;
    }
    if (!progressed) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-SET-PAYLOAD-V1",
                    "nested set-operation graph is cyclic or unresolved");
    }
  }
  if (total_set_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "nested set-operation work exceeds the admitted candidate "
                  "bound");
  }
  if (!CheckedAdd(memory_bytes, total_set_work, &memory_bytes)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "nested set-operation memory bound overflowed");
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "nested set-operation exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  std::unordered_map<std::string, std::string> set_capability_uuids;
  std::string graph_identity = "nested-set";
  for (const auto& node : graph.nodes) {
    const auto prepared = prepared_set_nodes.find(node.logical_node_id);
    if (prepared == prepared_set_nodes.end()) continue;
    graph_identity += "." + std::to_string(node.logical_node_id) + "." +
                      prepared->second.profile.identity_component;
    set_capability_uuids.try_emplace(
        prepared->second.profile.implementation_id,
        DerivedCanonicalUuid(
            identity_scope,
            "set." + prepared->second.profile.implementation_id +
                ".capability"));
  }

  std::vector<LivePhysicalNodeProfile> profiles;
  profiles.reserve(graph.nodes.size());
  for (const auto& node : graph.nodes) {
    if (node.node_kind ==
        plan::CanonicalLogicalRelationalNodeKind::kValues) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", row_bounds.at(node.logical_node_id),
           leaf_memory.at(node.logical_node_id), 0, 0});
      continue;
    }
    const auto& prepared = prepared_set_nodes.at(node.logical_node_id);
    profiles.push_back(
        {node.logical_node_id, prepared.profile.implementation_id,
         set_capability_uuids.at(prepared.profile.implementation_id),
         node.node_kind, exec::PhysicalNodeKind::kSetOperation,
         prepared.profile.physical_semantic_id,
         prepared.maximum_output_row_count, memory_bytes, 2, 2});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, graph_identity + ".selected-plan",
      "NESTED SET OPERATION");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
      "NESTED SET OPERATION");
  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));

  std::unordered_set<std::string> registered_implementations;
  for (const auto& [node_id, prepared] : prepared_set_nodes) {
    (void)node_id;
    if (!registered_implementations
             .insert(prepared.profile.implementation_id)
             .second) {
      continue;
    }
    exec::CanonicalPhysicalExecutorRegistration registration;
    registration.node_kind = exec::PhysicalNodeKind::kSetOperation;
    registration.implementation_id = prepared.profile.implementation_id;
    registration.executor_capability_uuid =
        set_capability_uuids.at(prepared.profile.implementation_id);
    registration.executor_capability_abi_version = 1;
    registration.engine_owned = true;
    registration.accepts_optimizer_publication_v2 = true;
    registration.execute =
        [prepared_set_nodes, mga_context = request.context](
            const exec::TypedPhysicalNodeDag& dag,
            const exec::PhysicalNodeRecord& node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
          exec::CanonicalPhysicalDispatchStepResult step;
          step.selected_plan_uuid = dag.selected_plan_uuid;
          step.mga_statement_context = dag.mga_statement_context;
          step.executed_physical_node_id = node.physical_node_id;
          step.causal_counter_id = node.causal_counter_id;
          step.output_descriptor_ids = node.output_descriptor_ids;
          step.authority.engine_mga_snapshot_bound = true;
          const auto prepared =
              prepared_set_nodes.find(node.relational_node_id);
          if (prepared == prepared_set_nodes.end() ||
              prepared->second.profile.implementation_id !=
                  node.implementation_id ||
              inputs.size() != 2 ||
              !inputs[0].materialized_output_batch.has_value() ||
              !inputs[1].materialized_output_batch.has_value()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
            step.diagnostic.detail =
                "nested set-operation executor input/profile is unresolved";
            return step;
          }
          const auto& config = prepared->second;
          exec::CanonicalSetOperationAllRequest set_request;
          set_request.physical_dag = dag;
          if (node.physical_node_id != dag.root_physical_node_id) {
            // The quantified set executor validates one binary operation as
            // its request root. Build a request-local ancestor view for this
            // already selected dispatch step; the optimizer publication and
            // outer physical DAG remain unchanged and are still consumed by
            // the engine dispatcher.
            std::unordered_set<std::uint64_t> execution_view_nodes;
            std::vector<std::uint64_t> execution_view_pending{
                node.physical_node_id};
            while (!execution_view_pending.empty()) {
              const auto physical_node_id = execution_view_pending.back();
              execution_view_pending.pop_back();
              if (!execution_view_nodes.insert(physical_node_id).second) {
                continue;
              }
              const auto found = std::ranges::find_if(
                  dag.nodes, [&](const auto& candidate) {
                    return candidate.physical_node_id == physical_node_id;
                  });
              if (found == dag.nodes.end()) {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "QOW-DIAG-RELATIONAL-LIVE-SET-INPUT-V1";
                step.diagnostic.detail =
                    "nested set-operation execution view is unresolved";
                return step;
              }
              execution_view_pending.insert(
                  execution_view_pending.end(),
                  found->input_physical_node_ids.begin(),
                  found->input_physical_node_ids.end());
            }
            std::erase_if(set_request.physical_dag.nodes,
                          [&](const auto& candidate) {
                            return !execution_view_nodes.contains(
                                candidate.physical_node_id);
                          });
            set_request.physical_dag.root_physical_node_id =
                node.physical_node_id;
          }
          set_request.selected_physical_node_id = node.physical_node_id;
          set_request.left_batch = *inputs[0].materialized_output_batch;
          set_request.right_batch = *inputs[1].materialized_output_batch;
          set_request.result_columns = config.prepared.result_columns;
          set_request.operation = config.profile.operation;
          set_request.alignment = config.profile.alignment;
          set_request.quantifier = config.profile.quantifier;
          set_request.equality_profile = config.profile.equality_profile;
          set_request.type_profile = config.profile.type_profile;
          set_request.collation_bindings =
              config.prepared.collation_bindings;
          set_request.maximum_equality_comparison_count =
              config.maximum_equality_comparison_count;
          set_request.maximum_output_row_count =
              std::max<std::size_t>(1, config.maximum_output_row_count);
          set_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context, dag);
          const auto set_result =
              config.profile.quantifier ==
                      exec::CanonicalSetOperationQuantifier::kAll
                  ? exec::ExecuteCanonicalSetOperationAll(set_request)
                  : exec::ExecuteCanonicalSetOperationDistinct(set_request);
          if (!set_result.diagnostic.ok) {
            step.diagnostic = set_result.diagnostic;
            return step;
          }
          step.result_handle_id = node.physical_node_id;
          step.input_row_count = set_result.left_input_row_count +
                                 set_result.right_input_row_count;
          step.rows_examined = step.input_row_count;
          step.output_row_count = set_result.output_batch.rows.size();
          step.materialized_output_batch = set_result.output_batch;
          step.mga_statement_context = set_result.mga_statement_context;
          return step;
        };
    execution_request.available_executors.push_back(std::move(registration));
  }

  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          graph_identity + ".execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      graph_identity + ".transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      prepared_set_nodes.at(root->logical_node_id).prepared.result_bindings;
  const auto root_output_bound =
      prepared_set_nodes.at(root->logical_node_id).maximum_output_row_count;
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, root_output_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SET-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "nested set-operation selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeJoinQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  std::optional<exec::CanonicalAcceptedJoinKind> join_kind;
  std::string join_component;
  std::string operation_name;
  if (root != graph.nodes.end()) {
    if (root->semantic_variant_id == "join.inner.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kInner;
      join_component = "inner";
      operation_name = "INNER JOIN";
    } else if (root->semantic_variant_id == "join.cross.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kCross;
      join_component = "cross";
      operation_name = "CROSS JOIN";
    } else if (root->semantic_variant_id == "join.left-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
      join_component = "left-outer";
      operation_name = "LEFT OUTER JOIN";
    } else if (root->semantic_variant_id == "join.right-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
      join_component = "right-outer";
      operation_name = "RIGHT OUTER JOIN";
    } else if (root->semantic_variant_id == "join.full-outer.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
      join_component = "full-outer";
      operation_name = "FULL OUTER JOIN";
    } else if (root->semantic_variant_id == "join.left-semi.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
      join_component = "left-semi";
      operation_name = "LEFT SEMI JOIN";
    } else if (root->semantic_variant_id == "join.left-anti.v1") {
      join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
      join_component = "left-anti";
      operation_name = "LEFT ANTI JOIN";
    }
  }
  const auto expected_expression_count =
      join_kind == exec::CanonicalAcceptedJoinKind::kCross ? 0U : 1U;
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      !join_kind.has_value() ||
      root->input_logical_node_ids.size() != 2 ||
      root->input_logical_node_ids[0] == root->input_logical_node_ids[1] ||
      root->bound_expression_ids.size() != expected_expression_count ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto left_node = find_node(root->input_logical_node_ids[0]);
  const auto right_node = find_node(root->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1") {
    return result;
  }
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty() ||
        (values && !node.input_logical_node_ids.empty())) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-ADMISSION-V1",
                  "live " + operation_name +
                      " execution lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  if (!left.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "left VALUES: " + left.detail);
  }
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!right.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  "right VALUES: " + right.detail);
  }
  auto prepared_root = PrepareJoinRoot(request.relational_dag, *root,
                                       *left_node, *right_node, left, right,
                                       *join_kind);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name +
                      " pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  if (pair_count >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " pair evaluation exceeds the admitted candidate bound");
  }

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name + " input size overflowed");
  }
  std::uint64_t total_memory = 0;
  std::uint64_t predicate_memory = 0;
  if (!CheckedAdd(left_memory, right_memory, &total_memory) ||
      !CheckedMultiply(pair_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(total_memory, predicate_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live " + operation_name +
                      " predicate state size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " predicate state exceeds the admitted memory budget");
  }

  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(pair_count);
  std::vector<bool> matched_left(left_count, false);
  std::vector<bool> matched_right(right_count, false);
  std::size_t matched_pair_count = 0;
  if (*join_kind == exec::CanonicalAcceptedJoinKind::kCross) {
    predicate_truth_values.assign(
        pair_count, api::EngineSqlTruthValue::true_value);
    std::fill(matched_left.begin(), matched_left.end(), right_count != 0);
    std::fill(matched_right.begin(), matched_right.end(), left_count != 0);
    matched_pair_count = pair_count;
  } else {
    CanonicalRelationalExpressionRuntime expression_runtime(
        request.relational_dag, request.expression_services);
    std::vector<api::EngineTypedValue> predicate_row_values;
    predicate_row_values.reserve(left.batch.columns.size() +
                                 right.batch.columns.size());
    for (std::size_t left_ordinal = 0; left_ordinal < left_count;
         ++left_ordinal) {
      for (std::size_t right_ordinal = 0; right_ordinal < right_count;
           ++right_ordinal) {
        predicate_row_values.clear();
        const auto& left_values = left.batch.rows[left_ordinal].values;
        const auto& right_values = right.batch.rows[right_ordinal].values;
        predicate_row_values.insert(predicate_row_values.end(),
                                    left_values.begin(), left_values.end());
        predicate_row_values.insert(predicate_row_values.end(),
                                    right_values.begin(), right_values.end());
        api::EngineSqlTruthValue predicate_truth =
            api::EngineSqlTruthValue::unknown;
        std::string predicate_detail;
        if (!expression_runtime.EvaluatePredicateForConsumer(
                prepared_root.predicate_expression_id,
                prepared_root.predicate_row_binding, predicate_row_values,
                api::EngineCanonicalExpressionConsumer::join,
                &predicate_truth, &predicate_detail)) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-JOIN-PAYLOAD-V1",
                        operation_name + " predicate pair " +
                            std::to_string(predicate_truth_values.size()) +
                            ": " + predicate_detail);
        }
        predicate_truth_values.push_back(predicate_truth);
        if (predicate_truth == api::EngineSqlTruthValue::true_value) {
          matched_left[left_ordinal] = true;
          matched_right[right_ordinal] = true;
          ++matched_pair_count;
        }
      }
    }
  }
  const auto matched_left_count = static_cast<std::size_t>(
      std::ranges::count(matched_left, true));
  const auto matched_right_count = static_cast<std::size_t>(
      std::ranges::count(matched_right, true));
  const auto unmatched_left_count = left_count - matched_left_count;
  const auto unmatched_right_count = right_count - matched_right_count;
  std::size_t output_row_bound = matched_pair_count;
  const auto add_output_rows = [&](const std::size_t additional) {
    if (additional > std::numeric_limits<std::size_t>::max() -
                         output_row_bound) {
      return false;
    }
    output_row_bound += additional;
    return true;
  };
  switch (*join_kind) {
    case exec::CanonicalAcceptedJoinKind::kLeftOuter:
      if (!add_output_rows(unmatched_left_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live LEFT OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kRightOuter:
      if (!add_output_rows(unmatched_right_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live RIGHT OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kFullOuter:
      if (!add_output_rows(unmatched_left_count) ||
          !add_output_rows(unmatched_right_count)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live FULL OUTER JOIN output cardinality overflowed");
      }
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftSemi:
      output_row_bound = matched_left_count;
      break;
    case exec::CanonicalAcceptedJoinKind::kLeftAnti:
      output_row_bound = unmatched_left_count;
      break;
    case exec::CanonicalAcceptedJoinKind::kCross:
    case exec::CanonicalAcceptedJoinKind::kInner:
      break;
  }

  if (output_row_bound != 0) {
    std::uint64_t output_memory = output_row_bound;
    const auto add_tuple_memory = [&](const exec::DescriptorTuple& tuple) {
      for (const auto& value : tuple.values) {
        if (!CheckedAdd(output_memory, value.encoded_value.size(),
                        &output_memory)) {
          return false;
        }
      }
      return true;
    };
    bool output_memory_valid = true;
    if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi) {
      for (std::size_t left_ordinal = 0;
           output_memory_valid && left_ordinal < left_count;
           ++left_ordinal) {
        if (matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
    } else if (*join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti) {
      for (std::size_t left_ordinal = 0;
           output_memory_valid && left_ordinal < left_count;
           ++left_ordinal) {
        if (!matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
    } else {
      const bool emits_unmatched_left =
          *join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
          *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
      const bool emits_unmatched_right =
          *join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
          *join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
      for (std::size_t pair = 0;
           output_memory_valid && pair < predicate_truth_values.size();
           ++pair) {
        if (predicate_truth_values[pair] !=
            api::EngineSqlTruthValue::true_value) {
          continue;
        }
        const auto left_ordinal = pair / right_count;
        const auto right_ordinal = pair % right_count;
        output_memory_valid =
            add_tuple_memory(left.batch.rows[left_ordinal]) &&
            add_tuple_memory(right.batch.rows[right_ordinal]);
      }
      for (std::size_t left_ordinal = 0;
           output_memory_valid && emits_unmatched_left &&
           left_ordinal < left_count;
           ++left_ordinal) {
        if (!matched_left[left_ordinal]) {
          output_memory_valid =
              add_tuple_memory(left.batch.rows[left_ordinal]);
        }
      }
      for (std::size_t right_ordinal = 0;
           output_memory_valid && emits_unmatched_right &&
           right_ordinal < right_count;
           ++right_ordinal) {
        if (!matched_right[right_ordinal]) {
          output_memory_valid =
              add_tuple_memory(right.batch.rows[right_ordinal]);
        }
      }
    }
    if (!output_memory_valid ||
        !CheckedAdd(total_memory, output_memory, &total_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live " + operation_name + " output size overflowed");
    }
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live " + operation_name +
                      " exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope,
                           "join." + join_component + ".capability");
  const auto join_implementation_id =
      "join." + join_component + ".3vl.nested.v1";
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    const bool values =
        node.node_kind == plan::CanonicalLogicalRelationalNodeKind::kValues;
    std::uint64_t node_rows = output_row_bound;
    std::uint64_t node_memory = total_memory;
    if (node.logical_node_id == left_node->logical_node_id) {
      node_rows = left_count;
      node_memory = left_memory;
    } else if (node.logical_node_id == right_node->logical_node_id) {
      node_rows = right_count;
      node_memory = right_memory;
    }
    profiles.push_back(
        {node.logical_node_id,
         values ? std::string(kValuesImplementationId)
                : join_implementation_id,
         values ? values_capability_uuid : join_capability_uuid,
         node.node_kind,
         values ? exec::PhysicalNodeKind::kValues
                : exec::PhysicalNodeKind::kJoin,
         values ? "canonical.values.materialize.v1"
                : "canonical." + join_implementation_id,
         node_rows,
         node_memory,
         values ? 0U : 2U,
         values ? 0U : 2U});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "join." + join_component + ".selected-plan",
      operation_name);
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-VALUES-V1", operation_name);

  auto join_registration = MakeLiveJoinRegistration(
      join_implementation_id, join_capability_uuid,
      std::move(predicate_truth_values), pair_count, output_row_bound,
      *join_kind, operation_name, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(std::move(join_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "join.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "join.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live INNER JOIN selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-041 through RCP-044: compose one accepted INNER JOIN with a
// row-dependent FILTER, computed PROJECT, optional query DISTINCT and SORT,
// and optional final LIMIT as one selected canonical physical DAG.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeInnerJoinFilterProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto find_node = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == node_id;
    });
  };
  const auto root = find_node(graph.root_logical_node_id);
  if (root == graph.nodes.end()) {
    return result;
  }
  const bool has_limit =
      root->node_kind == plan::CanonicalLogicalRelationalNodeKind::kLimit;
  const bool fetch_first_rows_only =
      has_limit &&
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
  const bool has_offset =
      has_limit && root->bound_expression_ids.size() == 2;
  if (has_limit &&
      ((root->semantic_variant_id != "limit.bound-count.v1" &&
        root->semantic_variant_id != "limit.bound-count-offset.v1" &&
        !fetch_first_rows_only) ||
       root->input_logical_node_ids.size() != 1 ||
       (root->semantic_variant_id == "limit.bound-count.v1"
            ? root->bound_expression_ids.size() != 1
            : root->bound_expression_ids.size() != 2))) {
    return result;
  }
  const auto sort_node =
      has_limit ? find_node(root->input_logical_node_ids.front()) : root;
  if (sort_node == graph.nodes.end()) {
    return result;
  }
  const bool has_sort =
      sort_node->node_kind == plan::CanonicalLogicalRelationalNodeKind::kSort;
  const auto sort_input_node =
      has_sort && sort_node->input_logical_node_ids.size() == 1
          ? find_node(sort_node->input_logical_node_ids.front())
          : graph.nodes.end();
  const bool has_distinct =
      has_sort && sort_input_node != graph.nodes.end() &&
      sort_input_node->node_kind ==
          plan::CanonicalLogicalRelationalNodeKind::kAggregate;
  if (has_distinct &&
      (!has_limit ||
       sort_input_node->semantic_variant_id !=
           "aggregate.query-distinct.v1" ||
       sort_input_node->input_logical_node_ids.size() != 1)) {
    return result;
  }
  if (has_offset && !has_distinct) {
    return result;
  }
  if ((has_limit && !has_sort) ||
      graph.nodes.size() !=
          (has_distinct ? 8U
                        : (has_limit ? 7U : (has_sort ? 6U : 5U))) ||
      (has_sort &&
       (sort_node->semantic_variant_id != "sort.required-order.v1" ||
        sort_node->input_logical_node_ids.size() != 1)) ||
      (!has_sort &&
       root->node_kind !=
           plan::CanonicalLogicalRelationalNodeKind::kProject) ||
      (!has_sort &&
       !request.optimizer_request.logical_properties.properties.empty())) {
    return result;
  }
  const auto project_input_node =
      has_distinct
          ? find_node(sort_input_node->input_logical_node_ids.front())
          : sort_input_node;
  const auto project_node = has_sort ? project_input_node : root;
  if (project_node == graph.nodes.end() ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      find_node(project_node->input_logical_node_ids.front());
  if (filter_node == graph.nodes.end() ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto join_node = find_node(filter_node->input_logical_node_ids.front());
  if (join_node == graph.nodes.end() ||
      join_node->node_kind != plan::CanonicalLogicalRelationalNodeKind::kJoin ||
      join_node->semantic_variant_id != "join.inner.v1" ||
      join_node->input_logical_node_ids.size() != 2 ||
      join_node->input_logical_node_ids[0] ==
          join_node->input_logical_node_ids[1] ||
      join_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto left_node = find_node(join_node->input_logical_node_ids[0]);
  const auto right_node = find_node(join_node->input_logical_node_ids[1]);
  if (left_node == graph.nodes.end() || right_node == graph.nodes.end() ||
      left_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      right_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      left_node->semantic_variant_id != "values.literal-table.v1" ||
      right_node->semantic_variant_id != "values.literal-table.v1" ||
      !left_node->input_logical_node_ids.empty() ||
      !right_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        ((!has_sort ||
          node.logical_node_id != sort_node->logical_node_id) &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  constexpr std::string_view kPayloadDiagnostic =
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-ADMISSION-V1",
        "INNER JOIN/FILTER/PROJECT lacks optimizer admission");
  }

  auto left = MaterializeValues(request.relational_dag, *left_node,
                                request.expression_services);
  auto right = MaterializeValues(request.relational_dag, *right_node,
                                 request.expression_services);
  if (!left.ok || !right.ok) {
    return refuse(std::string(kPayloadDiagnostic),
                  !left.ok ? "left VALUES: " + left.detail
                           : "right VALUES: " + right.detail);
  }
  auto prepared_join = PrepareJoinRoot(
      request.relational_dag, *join_node, *left_node, *right_node, left,
      right, exec::CanonicalAcceptedJoinKind::kInner);
  if (!prepared_join.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_join.detail);
  }

  const auto left_count = left.batch.rows.size();
  const auto right_count = right.batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT pair count overflowed");
  }
  const auto pair_count = left_count * right_count;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> join_truth_values;
  join_truth_values.reserve(pair_count);
  MaterializedValues joined_input;
  joined_input.ok = true;
  joined_input.batch.columns = left.batch.columns;
  joined_input.batch.columns.insert(joined_input.batch.columns.end(),
                                    right.batch.columns.begin(),
                                    right.batch.columns.end());
  joined_input.batch.rows.reserve(pair_count);
  joined_input.result_bindings = prepared_join.result_bindings;
  std::vector<api::EngineTypedValue> pair_values;
  pair_values.reserve(joined_input.batch.columns.size());
  for (const auto& left_row : left.batch.rows) {
    for (const auto& right_row : right.batch.rows) {
      pair_values.clear();
      pair_values.insert(pair_values.end(), left_row.values.begin(),
                         left_row.values.end());
      pair_values.insert(pair_values.end(), right_row.values.begin(),
                         right_row.values.end());
      api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
      std::string detail;
      if (!expression_runtime.EvaluatePredicateForConsumer(
              prepared_join.predicate_expression_id,
              prepared_join.predicate_row_binding, pair_values,
              api::EngineCanonicalExpressionConsumer::join, &truth,
              &detail)) {
        return refuse(std::string(kPayloadDiagnostic),
                      "INNER JOIN pair " +
                          std::to_string(join_truth_values.size()) + ": " +
                          detail);
      }
      join_truth_values.push_back(truth);
      if (truth == api::EngineSqlTruthValue::true_value) {
        exec::DescriptorTuple joined_row;
        joined_row.values = pair_values;
        joined_input.batch.rows.push_back(std::move(joined_row));
      }
    }
  }

  auto prepared_filter = PrepareFilterRoot(
      request.relational_dag, *filter_node, *join_node, joined_input);
  if (!prepared_filter.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_filter.detail);
  }
  const auto joined_row_count = joined_input.batch.rows.size();
  std::vector<api::EngineSqlTruthValue> filter_truth_values;
  filter_truth_values.reserve(joined_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = joined_input.batch.columns;
  filtered_input.batch.rows.reserve(joined_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : joined_input.batch.rows) {
    api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
    std::string detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter, &truth,
            &detail)) {
      return refuse(std::string(kPayloadDiagnostic),
                    "post-JOIN FILTER row " +
                        std::to_string(filter_truth_values.size()) + ": " +
                        detail);
    }
    filter_truth_values.push_back(truth);
    if (truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }

  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(std::string(kPayloadDiagnostic),
                  prepared_project.detail.empty()
                      ? "post-JOIN PROJECT is not an expression projection"
                      : prepared_project.detail);
  }
  PreparedDistinctRoot prepared_distinct;
  PreparedSortRoot prepared_sort;
  PreparedLimitRoot prepared_limit;
  MaterializedValues projected_input;
  if (has_sort) {
    projected_input.ok = true;
    projected_input.batch = prepared_project.expression_output_batch;
    projected_input.result_bindings = prepared_project.result_bindings;
    if (has_distinct) {
      prepared_distinct = PrepareQueryDistinctRoot(
          request.context, request.relational_dag, *sort_input_node,
          *project_node, projected_input);
      if (!prepared_distinct.ok) {
        return refuse(std::string(kPayloadDiagnostic),
                      prepared_distinct.detail);
      }
    }
    prepared_sort = PrepareSortRoot(
        request.context, request.relational_dag,
        request.optimizer_request.logical_properties, *sort_node,
        has_distinct ? *sort_input_node : *project_node, projected_input);
    if (!prepared_sort.ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_sort.detail);
    }
  }
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  if (has_limit) {
    prepared_limit = PrepareLimitRoot(
        request.relational_dag, *root, *sort_node, projected_input);
    if (!prepared_limit.ok) {
      return refuse(std::string(kPayloadDiagnostic), prepared_limit.detail);
    }
    std::string bound_detail;
    if (!EvaluateNonNegativeRowBound(
            &expression_runtime, root->bound_expression_ids.front(),
            &row_limit, &bound_detail) ||
        (has_offset &&
         !EvaluateNonNegativeRowBound(
             &expression_runtime, root->bound_expression_ids[1],
             &row_offset, &bound_detail))) {
      return refuse(std::string(kPayloadDiagnostic),
                    "LIMIT/FETCH bound: " + bound_detail);
    }
  }
  const auto filtered_row_count = filtered_input.batch.rows.size();
  std::uint64_t project_work = 0;
  std::uint64_t total_work = 0;
  if (!CheckedMultiply(filtered_row_count,
                       project_node->bound_expression_ids.size(),
                       &project_work) ||
      !CheckedAdd(pair_count, joined_row_count, &total_work) ||
      !CheckedAdd(total_work, project_work, &total_work) ||
      (has_limit &&
       !CheckedAdd(total_work, root->bound_expression_ids.size(),
                   &total_work))) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT work overflowed");
  }
  if (total_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "INNER JOIN/FILTER/PROJECT work exceeds the admitted "
                  "candidate bound");
  }

  std::uint64_t left_memory = 1;
  std::uint64_t right_memory = 1;
  std::uint64_t joined_memory = 1;
  std::uint64_t filtered_memory = 1;
  std::uint64_t projected_memory = 1;
  std::uint64_t join_state_memory = 0;
  std::uint64_t filter_state_memory = 0;
  std::uint64_t join_memory = 0;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t distinct_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(left.batch, &left_memory) ||
      !AddBatchMemoryBytes(right.batch, &right_memory) ||
      !AddBatchMemoryBytes(joined_input.batch, &joined_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &projected_memory) ||
      !CheckedMultiply(pair_count, sizeof(api::EngineSqlTruthValue),
                       &join_state_memory) ||
      !CheckedMultiply(joined_row_count, sizeof(api::EngineSqlTruthValue),
                       &filter_state_memory) ||
      !CheckedAdd(left_memory, right_memory, &join_memory) ||
      !CheckedAdd(join_memory, join_state_memory, &join_memory) ||
      !CheckedAdd(join_memory, joined_memory, &join_memory) ||
      !CheckedAdd(join_memory, filter_state_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, projected_memory, &project_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT memory overflowed");
  }
  if (has_distinct &&
      (!CheckedMultiply(filtered_row_count, filtered_row_count,
                        &distinct_comparison_count) ||
       !CheckedMultiply(
           distinct_comparison_count,
           prepared_project.expression_output_batch.columns.size(),
           &distinct_comparison_count) ||
       !CheckedMultiply(
           filtered_row_count,
           prepared_project.expression_output_batch.columns.size(),
           &distinct_self_comparison_count) ||
       !CheckedAdd(distinct_comparison_count,
                   distinct_self_comparison_count,
                   &distinct_comparison_count) ||
       !CheckedAdd(project_memory, projected_memory, &distinct_memory) ||
       !CheckedAdd(distinct_memory, distinct_comparison_count,
                   &distinct_memory) ||
       distinct_comparison_count >
           std::numeric_limits<std::size_t>::max())) {
    return refuse(
        "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
        "INNER JOIN/FILTER/PROJECT/DISTINCT memory or comparison count "
        "overflowed");
  }
  const auto pre_sort_memory =
      has_distinct ? distinct_memory : project_memory;
  if (has_sort &&
      (!CheckedMultiply(filtered_row_count, filtered_row_count,
                        &comparison_count) ||
       !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                        &row_order_memory) ||
       !CheckedAdd(pre_sort_memory, projected_memory, &sort_memory) ||
       !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
       !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
       comparison_count > std::numeric_limits<std::size_t>::max())) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT/SORT memory or comparison "
                  "count overflowed");
  }
  if (has_limit &&
      !CheckedAdd(sort_memory, projected_memory, &limit_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "INNER JOIN/FILTER/PROJECT/SORT/LIMIT memory overflowed");
  }
  const auto final_memory =
      has_limit ? limit_memory : (has_sort ? sort_memory : project_memory);
  if (final_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  has_limit
                      ? "INNER JOIN/FILTER/PROJECT/SORT/LIMIT exceeds the "
                        "admitted memory budget"
                  : has_sort
                      ? "INNER JOIN/FILTER/PROJECT/SORT exceeds the admitted "
                        "memory budget"
                      : "INNER JOIN/FILTER/PROJECT exceeds the admitted "
                        "memory budget");
  }

  const auto offset_bound =
      row_offset > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = filtered_row_count - offset_bound;
  const auto output_row_bound =
      has_limit
          ? (row_limit > remaining_bound
                 ? remaining_bound
                 : static_cast<std::size_t>(row_limit))
          : filtered_row_count;

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto join_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "join.inner.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "distinct.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid = DerivedCanonicalUuid(
      identity_scope,
      fetch_first_rows_only
          ? "fetch.capability"
          : (has_offset ? "limit-offset.capability" : "limit.capability"));
  const auto deterministic_tie_evidence_uuid =
      has_sort
          ? DerivedCanonicalUuid(
                identity_scope + ":" + prepared_sort.ordering_property_uuid,
                has_distinct
                    ? "inner-join-filter-project-distinct-sort."
                      "deterministic-tie"
                    : "inner-join-filter-project-sort.deterministic-tie")
          : std::string{};
  const std::string operation_name =
      fetch_first_rows_only
          ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/FETCH"
          : (has_offset
                 ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT/OFFSET"
                 : (has_distinct
                        ? "INNER JOIN/FILTER/PROJECT/DISTINCT/SORT/LIMIT"
                        : (has_limit
                               ? "INNER JOIN/FILTER/PROJECT/SORT/LIMIT"
                               : (has_sort
                                      ? "INNER JOIN/FILTER/PROJECT/SORT"
                                      : "INNER JOIN/FILTER/PROJECT"))));
  constexpr std::string_view kJoinImplementationId =
      "join.inner.3vl.nested.v1";
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
  const std::string limit_semantic_id =
      fetch_first_rows_only
          ? "canonical.fetch.first-rows-only-offset.filtered-projected-"
            "distinct-joined-order.v1"
          : (has_offset
                 ? "canonical.limit.bound-count-offset.filtered-projected-"
                   "distinct-joined-order.v1"
                 : "canonical.limit.filtered-projected-joined-order.v1");
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& node : graph.nodes) {
    if (node.logical_node_id == left_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", left_count, left_memory, 0, 0});
    } else if (node.logical_node_id == right_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kValuesImplementationId),
           values_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kValues,
           "canonical.values.materialize.v1", right_count, right_memory, 0,
           0});
    } else if (node.logical_node_id == join_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, std::string(kJoinImplementationId),
           join_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kJoin,
           "canonical.join.inner.3vl.nested.v1", joined_row_count,
           join_memory, 2, 2});
    } else if (node.logical_node_id == filter_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "filter.3vl.row.v1", filter_capability_uuid,
           node.node_kind, exec::PhysicalNodeKind::kFilter,
           "canonical.filter.joined-row.3vl.v1", filtered_row_count,
           filter_memory, 1, 1});
    } else if (node.logical_node_id == project_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "project.typed.expression-row.v1",
           project_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kProject,
           "canonical.project.filtered-joined-expression-row.v1",
           filtered_row_count, project_memory, 1, 1});
    } else if (has_distinct &&
               node.logical_node_id == sort_input_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "aggregate.query-distinct.typed.v1",
           distinct_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kAggregate,
           "canonical.aggregate.filtered-projected-joined-query-distinct.v1",
           filtered_row_count, distinct_memory, 1, 1});
    } else if (node.logical_node_id == sort_node->logical_node_id) {
      profiles.push_back(
          {node.logical_node_id, "sort.typed.terms.v1",
           sort_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kSort,
           "canonical.sort.filtered-projected-joined-expression.v1",
           filtered_row_count, sort_memory, 1, 1, {},
           {prepared_sort.ordering_property_uuid},
           {plan::CanonicalLogicalPropertyKind::kOrdering}});
    } else {
      profiles.push_back(
          {node.logical_node_id, limit_implementation_id,
           limit_capability_uuid, node.node_kind,
           exec::PhysicalNodeKind::kLimit, limit_semantic_id,
           output_row_bound, limit_memory, 1, 1});
    }
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only
          ? "inner-join-filter-project-distinct-sort-fetch.selected-plan"
          : (has_offset
                 ? "inner-join-filter-project-distinct-sort-limit-offset."
                   "selected-plan"
                 : (has_distinct
                        ? "inner-join-filter-project-distinct-sort-limit."
                          "selected-plan"
                        : (has_limit
                               ? "inner-join-filter-project-sort-limit."
                                 "selected-plan"
                               : (has_sort
                                      ? "inner-join-filter-project-sort."
                                        "selected-plan"
                                      : "inner-join-filter-project."
                                        "selected-plan")))),
      operation_name);
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(left_node->logical_node_id, std::move(left.batch));
  values_batches.emplace(right_node->logical_node_id, std::move(right.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-VALUES-V1",
      operation_name);
  auto join_registration = MakeLiveJoinRegistration(
      std::string(kJoinImplementationId), join_capability_uuid,
      std::move(join_truth_values), pair_count, joined_row_count,
      exec::CanonicalAcceptedJoinKind::kInner, "INNER JOIN", request.context);
  auto filter_registration = MakeLiveFilterRegistration(
      std::move(filter_truth_values), filter_capability_uuid,
      joined_row_count, request.context);
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(join_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  if (has_distinct) {
    execution_request.available_executors.push_back(
        MakeLiveQueryDistinctRegistration(
            std::move(prepared_distinct.equality_terms),
            distinct_capability_uuid, filtered_row_count,
            std::max<std::size_t>(
                1, static_cast<std::size_t>(distinct_comparison_count)),
            request.context));
  }
  if (has_sort) {
    execution_request.available_executors.push_back(
        MakeLiveSortRegistration(
            std::move(prepared_sort.order_terms),
            deterministic_tie_evidence_uuid, sort_capability_uuid,
            filtered_row_count,
            std::max<std::size_t>(
                1, static_cast<std::size_t>(comparison_count)),
            request.context));
  }
  if (has_limit) {
    execution_request.available_executors.push_back(
        MakeLiveLimitRegistration(
            limit_implementation_id, limit_capability_uuid, row_limit,
            row_offset, fetch_first_rows_only, filtered_row_count,
            request.context));
  }
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          fetch_first_rows_only
              ? "inner-join-filter-project-distinct-sort-fetch."
                "execution-attempt"
              : (has_offset
                     ? "inner-join-filter-project-distinct-sort-limit-offset."
                       "execution-attempt"
                     : (has_distinct
                            ? "inner-join-filter-project-distinct-sort-limit."
                              "execution-attempt"
                            : (has_limit
                                   ? "inner-join-filter-project-sort-limit."
                                     "execution-attempt"
                                   : (has_sort
                                          ? "inner-join-filter-project-sort."
                                            "execution-attempt"
                                          : "inner-join-filter-project."
                                            "execution-attempt")))));
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      fetch_first_rows_only
          ? "inner-join-filter-project-distinct-sort-fetch."
            "transaction-effect-unchanged"
          : (has_offset
                 ? "inner-join-filter-project-distinct-sort-limit-offset."
                   "transaction-effect-unchanged"
                 : (has_distinct
                        ? "inner-join-filter-project-distinct-sort-limit."
                          "transaction-effect-unchanged"
                        : (has_limit
                               ? "inner-join-filter-project-sort-limit."
                                 "transaction-effect-unchanged"
                               : (has_sort
                                      ? "inner-join-filter-project-sort."
                                        "transaction-effect-unchanged"
                                      : "inner-join-filter-project."
                                        "transaction-effect-unchanged")))));
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  if (has_limit) {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_limit.result_bindings);
  } else if (has_sort) {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_sort.result_bindings);
  } else {
    execution_request.result_publication_request.column_bindings =
        std::move(prepared_project.result_bindings);
  }
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-JOIN-FILTER-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? operation_name + " selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      root->semantic_variant_id != "filter.where.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-ADMISSION-V1",
                  "live FILTER execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  "FILTER input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareFilterRoot(request.relational_dag, *root,
                                         *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  if (input_row_count >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER row evaluation exceeds the admitted candidate bound");
  }
  std::uint64_t input_memory = 1;
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER input size overflowed");
  }
  std::uint64_t predicate_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER predicate state size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER predicate state exceeds the admitted memory budget");
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  std::size_t output_row_bound = 0;
  std::uint64_t output_memory = 0;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_root.predicate_expression_id,
            prepared_root.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PAYLOAD-V1",
                    "FILTER predicate row " +
                        std::to_string(predicate_truth_values.size()) +
                        ": " + predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth != api::EngineSqlTruthValue::true_value) continue;
    ++output_row_bound;
    if (!CheckedAdd(output_memory, 1, &output_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live FILTER output row count overflowed");
    }
    for (const auto& value : row.values) {
      if (!CheckedAdd(output_memory, value.encoded_value.size(),
                      &output_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live FILTER output payload overflowed");
      }
    }
  }
  if (!CheckedAdd(total_memory, output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live FILTER output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live FILTER exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       output_row_bound,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter.selected-plan", "FILTER");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-VALUES-V1", "FILTER");

  auto filter_registration = MakeLiveFilterRegistration(
      std::move(predicate_truth_values), filter_capability_uuid,
      input_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "filter.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live FILTER selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-ADMISSION-V1",
                  "live PROJECT execution lacks optimizer admission");
  }
  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  "PROJECT input VALUES: " + input.detail);
  }
  auto prepared_root = root->bound_expression_ids.empty()
      ? PrepareDescriptorDirectProjectRoot(
            request.relational_dag, *root, *input_node, input)
      : PrepareExpressionProjectRoot(
            request.relational_dag, *root, *input_node, input,
            request.expression_services);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  if (prepared_root.expression_projection &&
      input_row_count >
          request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live expression PROJECT row evaluation exceeds the "
                  "admitted candidate bound");
  }
  std::uint64_t input_memory = 1;
  std::uint64_t output_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      (prepared_root.expression_projection &&
       !AddBatchMemoryBytes(prepared_root.expression_output_batch,
                            &output_memory)) ||
      !CheckedAdd(input_memory,
                  prepared_root.expression_projection ? output_memory
                                                      : input_memory,
                  &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live PROJECT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const std::string project_implementation_id =
      prepared_root.expression_projection
          ? "project.typed.expression-row.v1"
          : "project.typed.row.v1";
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       project_implementation_id,
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       prepared_root.expression_projection
           ? "canonical.project.expression-row.v1"
           : "canonical.project.descriptor-direct.v1",
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project.selected-plan", "PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-VALUES-V1", "PROJECT");

  auto project_registration = MakeLiveProjectRegistration(
      prepared_root, project_implementation_id, project_capability_uuid,
      input_row_count, request.relational_dag, request.expression_services,
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live PROJECT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-035: one selected VALUES -> row-dependent FILTER -> computed PROJECT
// DAG. The PROJECT consumes only the physical FILTER batch, so rejected rows
// cannot be evaluated or recovered by the SELECT-list route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kProject ||
      root->semantic_variant_id != "project.select-list.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-ADMISSION-V1",
                  "FILTER/PROJECT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  "FILTER/PROJECT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRoot(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       root->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *root, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-PAYLOAD-V1",
                  prepared_project.detail.empty()
                      ? "FILTER/PROJECT requires an expression projection"
                      : prepared_project.detail);
  }

  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT materialization size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto filtered_row_count = filtered_input.batch.rows.size();
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {root->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project.selected-plan", "FILTER/PROJECT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-VALUES-V1",
      "FILTER/PROJECT");
  auto filter_registration = MakeLiveFilterRegistration(
      std::move(predicate_truth_values), filter_capability_uuid,
      input_row_count, request.context);
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_project.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, filtered_row_count);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-034: one selected VALUES -> computed PROJECT -> ORDER BY DAG. The
// projected scalar batch is the sole SORT input; source-only descriptors and
// caller-substituted payloads remain outside the route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeProjectSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 3 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == project_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != root->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-ADMISSION-V1",
                  "PROJECT/SORT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  "PROJECT/SORT input VALUES: " + input.detail);
  }
  std::uint64_t expression_work = 0;
  if (!CheckedMultiply(input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &expression_work) ||
      expression_work >
          request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PROJECT/SORT expression work exceeds the admitted "
                  "candidate bound");
  }
  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *project_node, *values_node, input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  prepared_project.detail.empty()
                      ? "PROJECT/SORT requires an expression projection"
                      : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *root, *project_node,
      projected_input);
  if (!prepared_sort.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-PAYLOAD-V1",
                  prepared_sort.detail);
  }

  const auto row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(row_count, row_count, &comparison_count) ||
      !CheckedMultiply(row_count, sizeof(std::size_t), &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "PROJECT/SORT materialization or comparison size "
                  "overflowed");
  }
  if (sort_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PROJECT/SORT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "project-sort.deterministic-tie");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       row_count,
       input_memory,
       0,
       0},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.expression-row.v1",
       row_count,
       project_memory,
       1,
       1},
      {root->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.projected-expression.v1",
       row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "project-sort.selected-plan", "PROJECT/SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-VALUES-V1", "PROJECT/SORT");
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid, row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "project-sort.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "project-sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_sort.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, row_count);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PROJECT-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "PROJECT/SORT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-036: one selected VALUES -> FILTER -> PROJECT -> SORT DAG. Each upper
// operator consumes only the materialized descriptor batch emitted by its
// immediate selected predecessor.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 4 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == project_node || values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != root->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-ADMISSION-V1",
        "FILTER/PROJECT/SORT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        "FILTER/PROJECT/SORT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRoot(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_project.detail.empty()
            ? "FILTER/PROJECT/SORT requires an expression projection"
            : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *root, *project_node,
      projected_input);
  if (!prepared_sort.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-PAYLOAD-V1",
        prepared_sort.detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT materialization or comparison size "
                  "overflowed");
  }
  if (sort_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-sort.deterministic-tie");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       project_memory,
       1,
       1},
      {root->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.filtered-projected-expression.v1",
       filtered_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project-sort.selected-plan",
      "FILTER/PROJECT/SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-VALUES-V1",
      "FILTER/PROJECT/SORT");
  auto filter_registration = MakeLiveFilterRegistration(
      std::move(predicate_truth_values), filter_capability_uuid,
      input_row_count, request.context);
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-sort.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_sort.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, filtered_row_count);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT/SORT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-037: append a bounded LIMIT to the accepted FILTER/PROJECT/SORT chain
// without republishing or re-planning any intermediate result.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 5 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      root->semantic_variant_id != "limit.bound-count.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto sort_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (sort_node == graph.nodes.end() || sort_node == root ||
      sort_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSort ||
      sort_node->semantic_variant_id != "sort.required-order.v1" ||
      sort_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == sort_node->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node == sort_node ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == sort_node || filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == project_node ||
      values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != sort_node->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-ADMISSION-V1",
        "FILTER/PROJECT/SORT/LIMIT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        "FILTER/PROJECT/SORT/LIMIT input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRoot(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
          "FILTER predicate row " +
              std::to_string(predicate_truth_values.size()) + ": " +
              predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work) ||
      !CheckedAdd(total_expression_work, root->bound_expression_ids.size(),
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT/LIMIT expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT/LIMIT expression work exceeds the "
                  "admitted candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_project.detail.empty()
            ? "FILTER/PROJECT/SORT/LIMIT requires an expression projection"
            : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_sort = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *project_node, projected_input);
  if (!prepared_sort.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRoot(
      request.relational_dag, *root, *sort_node, projected_input);
  if (!prepared_limit.ok) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        prepared_limit.detail);
  }
  std::uint64_t row_limit = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids.front(), &row_limit,
          &bound_detail)) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-PAYLOAD-V1",
        "LIMIT bound: " + bound_detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  const auto output_row_bound =
      row_limit > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(project_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, project_output_memory, &limit_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "FILTER/PROJECT/SORT/LIMIT materialization or comparison "
                  "size overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "FILTER/PROJECT/SORT/LIMIT exceeds the admitted memory "
                  "budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "limit.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-sort-limit.deterministic-tie");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {filter_node->logical_node_id,
       "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter,
       "canonical.filter.3vl.row.v1",
       filtered_row_count,
       filter_memory,
       1,
       1},
      {project_node->logical_node_id,
       "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1",
       filtered_row_count,
       project_memory,
       1,
       1},
      {sort_node->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.filtered-projected-expression.v1",
       filtered_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id,
       "limit.typed.v1",
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       "canonical.limit.filtered-projected-order.v1",
       output_row_bound,
       limit_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "filter-project-sort-limit.selected-plan",
      "FILTER/PROJECT/SORT/LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-VALUES-V1",
      "FILTER/PROJECT/SORT/LIMIT");
  auto filter_registration = MakeLiveFilterRegistration(
      std::move(predicate_truth_values), filter_capability_uuid,
      input_row_count, request.context);
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      "limit.typed.v1", limit_capability_uuid, row_limit, 0, false,
      filtered_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-sort-limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_limit.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-SORT-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "FILTER/PROJECT/SORT/LIMIT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// RCP-038: add full projected-row DISTINCT before ORDER BY and LIMIT in the
// accepted filtered SQL tail.
// RCP-039: carry the already signed LIMIT/OFFSET and FETCH FIRST ROWS ONLY
// profiles through the same exact six-node causal route.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeFilterProjectDistinctSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 6 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      (root->semantic_variant_id != "limit.bound-count.v1" &&
       root->semantic_variant_id != "limit.bound-count-offset.v1" &&
       root->semantic_variant_id !=
           "fetch.first-rows-only-offset.v1") ||
      root->input_logical_node_ids.size() != 1 ||
      (root->semantic_variant_id == "limit.bound-count.v1"
           ? root->bound_expression_ids.size() != 1
           : root->bound_expression_ids.size() != 2)) {
    return result;
  }
  const bool fetch_first_rows_only =
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
  const bool has_offset = root->bound_expression_ids.size() == 2;
  const auto sort_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (sort_node == graph.nodes.end() || sort_node == root ||
      sort_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSort ||
      sort_node->semantic_variant_id != "sort.required-order.v1" ||
      sort_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto distinct_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == sort_node->input_logical_node_ids.front();
      });
  if (distinct_node == graph.nodes.end() || distinct_node == root ||
      distinct_node == sort_node ||
      distinct_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      distinct_node->semantic_variant_id != "aggregate.query-distinct.v1" ||
      distinct_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto project_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               distinct_node->input_logical_node_ids.front();
      });
  if (project_node == graph.nodes.end() || project_node == root ||
      project_node == sort_node || project_node == distinct_node ||
      project_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kProject ||
      project_node->semantic_variant_id != "project.select-list.v1" ||
      project_node->input_logical_node_ids.size() != 1 ||
      project_node->bound_expression_ids.empty()) {
    return result;
  }
  const auto filter_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               project_node->input_logical_node_ids.front();
      });
  if (filter_node == graph.nodes.end() || filter_node == root ||
      filter_node == sort_node || filter_node == distinct_node ||
      filter_node == project_node ||
      filter_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kFilter ||
      filter_node->semantic_variant_id != "filter.where.v1" ||
      filter_node->input_logical_node_ids.size() != 1 ||
      filter_node->bound_expression_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               filter_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == distinct_node ||
      values_node == project_node || values_node == filter_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id != sort_node->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  constexpr std::string_view kPayloadDiagnostic =
      "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-PAYLOAD-V1";
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-ADMISSION-V1",
        "FILTER/PROJECT/DISTINCT/SORT/LIMIT lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse(std::string(kPayloadDiagnostic),
                  "full SQL tail input VALUES: " + input.detail);
  }
  auto prepared_filter = PrepareFilterRoot(
      request.relational_dag, *filter_node, *values_node, input);
  if (!prepared_filter.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_filter.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<api::EngineSqlTruthValue> predicate_truth_values;
  predicate_truth_values.reserve(input_row_count);
  MaterializedValues filtered_input;
  filtered_input.ok = true;
  filtered_input.batch.columns = input.batch.columns;
  filtered_input.batch.rows.reserve(input_row_count);
  filtered_input.result_bindings = prepared_filter.result_bindings;
  for (const auto& row : input.batch.rows) {
    api::EngineSqlTruthValue predicate_truth =
        api::EngineSqlTruthValue::unknown;
    std::string predicate_detail;
    if (!expression_runtime.EvaluatePredicateForConsumer(
            prepared_filter.predicate_expression_id,
            prepared_filter.predicate_row_binding, row.values,
            api::EngineCanonicalExpressionConsumer::filter,
            &predicate_truth, &predicate_detail)) {
      return refuse(std::string(kPayloadDiagnostic),
                    "FILTER predicate row " +
                        std::to_string(predicate_truth_values.size()) + ": " +
                        predicate_detail);
    }
    predicate_truth_values.push_back(predicate_truth);
    if (predicate_truth == api::EngineSqlTruthValue::true_value) {
      filtered_input.batch.rows.push_back(row);
    }
  }
  std::uint64_t project_expression_work = 0;
  std::uint64_t total_expression_work = 0;
  if (!CheckedMultiply(filtered_input.batch.rows.size(),
                       project_node->bound_expression_ids.size(),
                       &project_expression_work) ||
      !CheckedAdd(input_row_count, project_expression_work,
                  &total_expression_work) ||
      !CheckedAdd(total_expression_work, root->bound_expression_ids.size(),
                  &total_expression_work)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "full SQL-tail expression work overflowed");
  }
  if (total_expression_work >
      request.optimizer_request.resource.maximum_candidate_count) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "full SQL-tail expression work exceeds the admitted "
                  "candidate bound");
  }

  auto prepared_project = PrepareExpressionProjectRoot(
      request.relational_dag, *project_node, *filter_node, filtered_input,
      request.expression_services);
  if (!prepared_project.ok || !prepared_project.expression_projection) {
    return refuse(std::string(kPayloadDiagnostic),
                  prepared_project.detail.empty()
                      ? "full SQL tail requires an expression projection"
                      : prepared_project.detail);
  }
  MaterializedValues projected_input;
  projected_input.ok = true;
  projected_input.batch = prepared_project.expression_output_batch;
  projected_input.result_bindings = prepared_project.result_bindings;
  auto prepared_distinct = PrepareQueryDistinctRoot(
      request.context, request.relational_dag, *distinct_node, *project_node,
      projected_input);
  if (!prepared_distinct.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_distinct.detail);
  }
  auto prepared_sort = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *distinct_node, projected_input);
  if (!prepared_sort.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRoot(
      request.relational_dag, *root, *sort_node, projected_input);
  if (!prepared_limit.ok) {
    return refuse(std::string(kPayloadDiagnostic), prepared_limit.detail);
  }
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids.front(), &row_limit,
          &bound_detail) ||
      (has_offset &&
       !EvaluateNonNegativeRowBound(
           &expression_runtime, root->bound_expression_ids[1], &row_offset,
           &bound_detail))) {
    return refuse(std::string(kPayloadDiagnostic),
                  "LIMIT/FETCH bound: " + bound_detail);
  }

  const auto filtered_row_count = filtered_input.batch.rows.size();
  const auto offset_bound =
      row_offset > filtered_row_count
          ? filtered_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = filtered_row_count - offset_bound;
  const auto output_row_bound =
      row_limit > remaining_bound
          ? remaining_bound
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t predicate_memory = 0;
  std::uint64_t filtered_memory = 1;
  std::uint64_t project_output_memory = 1;
  std::uint64_t filter_memory = 0;
  std::uint64_t project_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t distinct_memory = 0;
  std::uint64_t sort_comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, sizeof(api::EngineSqlTruthValue),
                       &predicate_memory) ||
      !AddBatchMemoryBytes(filtered_input.batch, &filtered_memory) ||
      !AddBatchMemoryBytes(prepared_project.expression_output_batch,
                           &project_output_memory) ||
      !CheckedAdd(input_memory, predicate_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, filtered_memory, &filter_memory) ||
      !CheckedAdd(filter_memory, project_output_memory, &project_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &distinct_comparison_count) ||
      !CheckedMultiply(distinct_comparison_count,
                       projected_input.batch.columns.size(),
                       &distinct_comparison_count) ||
      !CheckedMultiply(filtered_row_count,
                       projected_input.batch.columns.size(),
                       &distinct_self_comparison_count) ||
      !CheckedAdd(distinct_comparison_count,
                  distinct_self_comparison_count,
                  &distinct_comparison_count) ||
      !CheckedAdd(project_memory, project_output_memory, &distinct_memory) ||
      !CheckedAdd(distinct_memory, distinct_comparison_count,
                  &distinct_memory) ||
      !CheckedMultiply(filtered_row_count, filtered_row_count,
                       &sort_comparison_count) ||
      !CheckedMultiply(filtered_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(distinct_memory, project_output_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, sort_comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, project_output_memory, &limit_memory) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      sort_comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "full SQL-tail materialization or comparison size "
                  "overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "full SQL tail exceeds the admitted memory budget");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "filter.capability");
  const auto project_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "project.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "distinct.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid = DerivedCanonicalUuid(
      identity_scope,
      fetch_first_rows_only
          ? "fetch.capability"
          : (has_offset ? "limit-offset.capability" : "limit.capability"));
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "filter-project-distinct-sort-limit.deterministic-tie");
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
  const std::string limit_semantic_id =
      fetch_first_rows_only
          ? "canonical.fetch.first-rows-only-offset.filtered-projected-"
            "distinct-order.v1"
          : (has_offset
                 ? "canonical.limit.bound-count-offset.filtered-projected-"
                   "distinct-order.v1"
                 : "canonical.limit.filtered-projected-distinct-order.v1");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id, std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues, "canonical.values.materialize.v1",
       input_row_count, input_memory, 0, 0},
      {filter_node->logical_node_id, "filter.3vl.row.v1",
       filter_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kFilter,
       exec::PhysicalNodeKind::kFilter, "canonical.filter.3vl.row.v1",
       filtered_row_count, filter_memory, 1, 1},
      {project_node->logical_node_id, "project.typed.expression-row.v1",
       project_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kProject,
       exec::PhysicalNodeKind::kProject,
       "canonical.project.filtered-expression-row.v1", filtered_row_count,
       project_memory, 1, 1},
      {distinct_node->logical_node_id, "aggregate.query-distinct.typed.v1",
       distinct_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       "canonical.aggregate.projected-query-distinct.v1", filtered_row_count,
       distinct_memory, 1, 1},
      {sort_node->logical_node_id, "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.distinct-projected-expression.v1", filtered_row_count,
       sort_memory, 1, 1, {}, {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id, limit_implementation_id, limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       limit_semantic_id, output_row_bound, limit_memory, 1, 1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only
          ? "filter-project-distinct-sort-fetch.selected-plan"
          : (has_offset
                 ? "filter-project-distinct-sort-limit-offset.selected-plan"
                 : "filter-project-distinct-sort-limit.selected-plan"),
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT/FETCH");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-FULL-SQL-TAIL-VALUES-V1",
      "FILTER/PROJECT/DISTINCT/SORT/LIMIT/FETCH");
  auto filter_registration = MakeLiveFilterRegistration(
      std::move(predicate_truth_values), filter_capability_uuid,
      input_row_count, request.context);
  auto project_registration = MakeLiveProjectRegistration(
      prepared_project, "project.typed.expression-row.v1",
      project_capability_uuid, filtered_row_count, request.relational_dag,
      request.expression_services, request.context);
  auto distinct_registration = MakeLiveQueryDistinctRegistration(
      std::move(prepared_distinct.equality_terms), distinct_capability_uuid,
      filtered_row_count,
      std::max<std::size_t>(
          1, static_cast<std::size_t>(distinct_comparison_count)),
      request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms), deterministic_tie_evidence_uuid,
      sort_capability_uuid, filtered_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(sort_comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      limit_implementation_id, limit_capability_uuid, row_limit, row_offset,
      fetch_first_rows_only, filtered_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(filter_registration));
  execution_request.available_executors.push_back(
      std::move(project_registration));
  execution_request.available_executors.push_back(
      std::move(distinct_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "filter-project-distinct-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "filter-project-distinct-sort-limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_limit.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-FILTER-PROJECT-DISTINCT-SORT-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "full selected SQL-tail DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGroupedCountSumQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (root == graph.nodes.end()) {
    return result;
  }
  const bool has_having =
      root->node_kind == plan::CanonicalLogicalRelationalNodeKind::kFilter &&
      IsLiveGroupedHavingProfile(root->semantic_variant_id);
  auto aggregate_root = root;
  if (has_having) {
    if (root->input_logical_node_ids.size() != 1) return result;
    aggregate_root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
      return node.logical_node_id == root->input_logical_node_ids.front();
    });
  }
  const auto profile =
      aggregate_root == graph.nodes.end()
          ? LiveGroupedCountSumProfile{}
          : MatchLiveGroupedCountSumProfile(
                aggregate_root->semantic_variant_id);
  if (aggregate_root == graph.nodes.end() ||
      aggregate_root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      (!has_having && root != aggregate_root) || !profile.matched) {
    return result;
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };

  const auto grouping_projection_count =
      profile.projects_grouping_metadata ? profile.key_count + 1 : 0;
  const auto expected_output_count =
      profile.key_count + 2 + grouping_projection_count;
  if (graph.nodes.size() != (has_having ? 3 : 2) ||
      aggregate_root->input_logical_node_ids.size() != 1 ||
      aggregate_root->bound_expression_ids.size() != expected_output_count ||
      aggregate_root->output_descriptor_ids.size() != expected_output_count ||
      (has_having &&
       (root->bound_expression_ids.size() != 1 ||
        root->output_descriptor_ids != aggregate_root->output_descriptor_ids)) ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM root shape is not exact");
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               aggregate_root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == aggregate_root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM input is not one literal VALUES leaf");
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
          "grouped COUNT/SUM does not admit object or property authority");
    }
  }
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-ADMISSION-V1",
        "live grouped COUNT/SUM execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  "grouped COUNT/SUM input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareGroupedCountSumRoot(
      request.relational_dag, *aggregate_root, *input_node, input, profile);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-PAYLOAD-V1",
                  prepared_root.detail);
  }
  PreparedGroupedHavingRoot prepared_having;
  if (has_having) {
    prepared_having = PrepareGroupedHavingRoot(
        request.relational_dag, *root, *aggregate_root, *input_node,
        prepared_root);
    if (!prepared_having.ok) {
      return refuse(
          "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-PAYLOAD-V1",
          prepared_having.detail);
    }
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t output_row_bound = 0;
  std::uint64_t per_group_memory = 0;
  std::uint64_t output_memory = 0;
  std::uint64_t projection_memory = 0;
  std::uint64_t total_memory = 0;
  constexpr std::uint64_t kGroupedStateOverhead = 256;
  constexpr std::uint64_t kGroupingProjectionBytes = 64;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(
          std::max<std::uint64_t>(1, input_row_count),
          static_cast<std::uint64_t>(prepared_root.grouping_sets.size()),
          &output_row_bound) ||
      !CheckedMultiply(output_row_bound,
                       kGroupedStateOverhead, &per_group_memory) ||
      !CheckedMultiply(
          input_memory,
          static_cast<std::uint64_t>(prepared_root.grouping_sets.size()),
          &output_memory) ||
      !CheckedMultiply(
          output_row_bound,
          static_cast<std::uint64_t>(grouping_projection_count) *
              kGroupingProjectionBytes,
          &projection_memory) ||
      !CheckedAdd(input_memory, per_group_memory, &total_memory) ||
      !CheckedAdd(total_memory, output_memory, &total_memory) ||
      !CheckedAdd(total_memory, projection_memory, &total_memory) ||
      output_row_bound > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live grouped COUNT/SUM memory size overflowed");
  }
  const auto maximum_output_rows =
      static_cast<std::size_t>(output_row_bound);
  std::uint64_t filter_truth_memory = 0;
  std::uint64_t filter_memory = total_memory;
  if ((has_having &&
       (!CheckedMultiply(output_row_bound,
                         sizeof(api::EngineSqlTruthValue),
                         &filter_truth_memory) ||
        !CheckedAdd(total_memory, filter_truth_memory, &filter_memory))) ||
      total_memory >
          request.optimizer_request.resource.memory_budget_bytes ||
      filter_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live grouped COUNT/SUM exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto aggregate_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "grouped-aggregate.capability");
  const auto filter_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "grouped-having.capability");
  std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id, std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues, "canonical.values.materialize.v1",
       input_row_count, input_memory, 0, 0},
      {aggregate_root->logical_node_id, "aggregate.registry-grouping-sets.v1",
       aggregate_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate, profile.transformation_id,
       output_row_bound, total_memory, 1, 1}};
  if (has_having) {
    profiles.push_back(
        {root->logical_node_id, "filter.3vl.row.v1", filter_capability_uuid,
         plan::CanonicalLogicalRelationalNodeKind::kFilter,
         exec::PhysicalNodeKind::kFilter,
         root->semantic_variant_id ==
                 "filter.having-not-not-sum-count-or-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-sum-count-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-sum-or-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-count-sum-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-not-not-count-sum-and-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-count-gt-int64-literal.v1"
             ? "canonical.filter.having-not-not-count-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-not-sum-gt-int64-literal.v1"
             ? "canonical.filter.having-not-not-sum-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-not-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-not-count-sum-and-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                       "filter.having-not-count-sum-or-gt-int64-literals.v1"
                   ? "canonical.filter.having-not-count-sum-or-gt-int64-literals.v1"
             : root->semantic_variant_id ==
                       "filter.having-not-count-gt-int64-literal.v1"
                   ? "canonical.filter.having-not-count-gt-int64-literal.v1"
             : root->semantic_variant_id ==
                 "filter.having-count-sum-and-gt-int64-literals.v1"
             ? "canonical.filter.having-count-sum-and-gt-int64-literals.v1"
             : (root->semantic_variant_id ==
                        "filter.having-count-sum-or-gt-int64-literals.v1"
                    ? "canonical.filter.having-count-sum-or-gt-int64-literals.v1"
                    : (root->semantic_variant_id ==
                               "filter.having-not-sum-gt-int64-literal.v1"
                           ? "canonical.filter.having-not-sum-gt-int64-literal.v1"
                           : "canonical.filter.having-sum-gt-int64-literal.v1")),
         output_row_bound, filter_memory, 1, 1});
  }
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      has_having ? "grouped-having.selected-plan"
                 : "grouped-aggregate.selected-plan",
      has_having ? "GROUPED HAVING" : "GROUPED AGGREGATE");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-VALUES-V1",
      "GROUPED AGGREGATE");

  exec::CanonicalPhysicalExecutorRegistration aggregate_registration;
  aggregate_registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate_registration.implementation_id =
      "aggregate.registry-grouping-sets.v1";
  aggregate_registration.executor_capability_uuid = aggregate_capability_uuid;
  aggregate_registration.executor_capability_abi_version = 1;
  aggregate_registration.engine_owned = true;
  aggregate_registration.accepts_optimizer_publication_v2 = true;
  aggregate_registration.execute =
      [prepared_root, input_row_count, maximum_output_rows, has_having,
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM cardinality differs from selected cost";
          return step;
        }

        const auto make_aggregate = [](const PreparedGlobalAggregateRoot& prepared) {
          exec::CanonicalAggregateRuntimeRequest aggregate;
          aggregate.descriptor = prepared.aggregate_descriptor;
          aggregate.value_columns = prepared.value_columns;
          aggregate.value_expression_descriptor_ids =
              prepared.value_descriptor_ids;
          aggregate.direct_arguments = prepared.direct_arguments;
          aggregate.result_column = prepared.result_column;
          aggregate.distinct = prepared.distinct;
          aggregate.aggregate_order_terms = prepared.aggregate_order_terms;
          aggregate.aggregate_separator = prepared.aggregate_separator;
          aggregate.listagg_overflow_mode = prepared.listagg_overflow_mode;
          aggregate.listagg_max_output_bytes =
              prepared.listagg_max_output_bytes;
          aggregate.listagg_truncation_indicator =
              prepared.listagg_truncation_indicator;
          aggregate.listagg_with_count = prepared.listagg_with_count;
          aggregate.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          return aggregate;
        };

        exec::CanonicalGroupedAggregateSetRuntimeRequest grouped_request;
        auto& first = grouped_request.first_aggregate;
        first.aggregate_request = make_aggregate(prepared_root.count);
        auto grouped_runtime_dag = dag;
        if (has_having) {
          const auto physical_filter = std::ranges::find_if(
              grouped_runtime_dag.nodes, [&](const auto& candidate) {
                return candidate.physical_node_id ==
                           grouped_runtime_dag.root_physical_node_id &&
                       candidate.node_kind == exec::PhysicalNodeKind::kFilter;
              });
          if (physical_filter == grouped_runtime_dag.nodes.end()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-INPUT-V1";
            step.diagnostic.detail =
                "grouped HAVING physical FILTER root is missing";
            return step;
          }
          grouped_runtime_dag.nodes.erase(physical_filter);
          grouped_runtime_dag.root_physical_node_id = node.physical_node_id;
        }
        if (!prepared_root.grouping_projection_columns.empty()) {
          const auto runtime_node = std::ranges::find_if(
              grouped_runtime_dag.nodes, [&](const auto& candidate) {
                return candidate.physical_node_id == node.physical_node_id;
              });
          if (runtime_node == grouped_runtime_dag.nodes.end() ||
              runtime_node->output_descriptor_ids.size() !=
                  prepared_root.key_terms.size() + 2 +
                      prepared_root.grouping_projection_columns.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouping projection physical descriptor shape drifted";
            return step;
          }
          runtime_node->output_descriptor_ids.resize(
              prepared_root.key_terms.size() + 2);
        }
        first.aggregate_request.physical_dag = std::move(grouped_runtime_dag);
        first.aggregate_request.selected_physical_node_id =
            node.physical_node_id;
        first.aggregate_request.input_batch = input_batch;
        first.aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context,
                                                first.aggregate_request.physical_dag);
        first.group_key_terms = prepared_root.key_terms;
        first.group_result_columns = prepared_root.key_result_columns;
        first.grouping_sets = prepared_root.grouping_sets;
        first.maximum_group_count = maximum_output_rows;
        first.maximum_output_rows = maximum_output_rows;
        auto additional_sum = make_aggregate(prepared_root.sum);
        // Grouped-set runtimes deliberately share the first aggregate's DAG,
        // selected node, and input batch.  The additional specification still
        // has to carry the exact same statement authority so it cannot become
        // a stale or detached participant in that shared execution.
        additional_sum.mga_authority = first.aggregate_request.mga_authority;
        grouped_request.additional_aggregates = {std::move(additional_sum)};

        auto aggregate_result =
            exec::ExecuteCanonicalGroupedAggregateSetRuntime(grouped_request);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = aggregate_result.diagnostic;
          return step;
        }
        if (!aggregate_result.group_identity_proven ||
            !aggregate_result.shared_state_authority_used ||
            aggregate_result.aggregate_count != 2 ||
            aggregate_result.groups.size() !=
                aggregate_result.output_batch.rows.size() ||
            aggregate_result.output_batch.rows.size() >
                maximum_output_rows) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM runtime did not prove shared group identity";
          return step;
        }
        std::vector<bool> grouping_sets_observed(
            prepared_root.grouping_sets.size(), false);
        for (const auto& group : aggregate_result.groups) {
          if (group.grouping_set_ordinal >=
                  prepared_root.grouping_sets.size() ||
              group.grouping_indicators.size() !=
                  prepared_root.key_terms.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouped COUNT/SUM runtime returned invalid grouping metadata";
            return step;
          }
          grouping_sets_observed[group.grouping_set_ordinal] = true;
          const auto& grouping_set = prepared_root.grouping_sets[
              group.grouping_set_ordinal];
          const auto expected_metadata =
              exec::ComputeCanonicalAggregateGroupingMetadata(
                  prepared_root.key_terms.size(), grouping_set);
          if (!expected_metadata.diagnostic.ok ||
              group.grouping_indicators !=
                  expected_metadata.grouping_indicators ||
              group.grouping_id != expected_metadata.grouping_id) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouped COUNT/SUM grouping metadata identity drifted";
            return step;
          }
        }
        if (std::ranges::find(grouping_sets_observed, false) !=
            grouping_sets_observed.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM runtime omitted a grouping-set identity";
          return step;
        }
        if (!prepared_root.grouping_projection_columns.empty()) {
          if (prepared_root.grouping_projection_columns.size() !=
                  prepared_root.key_terms.size() + 1 ||
              aggregate_result.output_batch.columns.size() !=
                  prepared_root.key_terms.size() + 2) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "grouping projection result descriptor shape drifted";
            return step;
          }
          aggregate_result.output_batch.columns.insert(
              aggregate_result.output_batch.columns.end(),
              prepared_root.grouping_projection_columns.begin(),
              prepared_root.grouping_projection_columns.end());
          for (std::size_t group_ordinal = 0;
               group_ordinal < aggregate_result.groups.size();
               ++group_ordinal) {
            auto& output_row =
                aggregate_result.output_batch.rows[group_ordinal];
            const auto& metadata = aggregate_result.groups[group_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared_root.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared_root.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  metadata.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              output_row.values.push_back(std::move(indicator));
            }
            if (metadata.grouping_id >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "GROUPING_ID exceeds the bound int64 result domain";
              return step;
            }
            api::EngineTypedValue grouping_id;
            grouping_id.descriptor =
                prepared_root.grouping_projection_columns.back().descriptor;
            grouping_id.encoded_value =
                std::to_string(metadata.grouping_id);
            grouping_id.state = api::EngineValueState::value;
            output_row.values.push_back(std::move(grouping_id));
          }
          const auto projected_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  aggregate_result.output_batch, node.output_descriptor_ids);
          if (!projected_validation.ok) {
            step.diagnostic = projected_validation;
            return step;
          }
        }
        step.authority = aggregate_result.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch = aggregate_result.output_batch;
        step.mga_statement_context = aggregate_result.mga_statement_context;
        return step;
      };

  exec::CanonicalPhysicalExecutorRegistration having_registration;
  if (has_having) {
    having_registration.node_kind = exec::PhysicalNodeKind::kFilter;
    having_registration.implementation_id = "filter.3vl.row.v1";
    having_registration.executor_capability_uuid = filter_capability_uuid;
    having_registration.executor_capability_abi_version = 1;
    having_registration.engine_owned = true;
    having_registration.accepts_optimizer_publication_v2 = true;
    having_registration.execute =
        [prepared_having, maximum_output_rows,
         relational_dag = request.relational_dag,
         expression_services = request.expression_services,
         mga_context = request.context](
            const exec::TypedPhysicalNodeDag& dag,
            const exec::PhysicalNodeRecord& node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
          exec::CanonicalPhysicalDispatchStepResult step;
          step.selected_plan_uuid = dag.selected_plan_uuid;
          step.mga_statement_context = dag.mga_statement_context;
          step.executed_physical_node_id = node.physical_node_id;
          step.causal_counter_id = node.causal_counter_id;
          step.output_descriptor_ids = node.output_descriptor_ids;
          step.authority.engine_mga_snapshot_bound = true;
          if (inputs.size() != 1 ||
              !inputs.front().materialized_output_batch.has_value()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-INPUT-V1";
            step.diagnostic.detail =
                "HAVING executor did not receive one grouped aggregate batch";
            return step;
          }
          const auto& input_batch = *inputs.front().materialized_output_batch;
          if (input_batch.rows.size() > maximum_output_rows ||
              input_batch.columns.size() !=
                  prepared_having.output_column_count ||
              node.output_descriptor_ids.size() !=
                  prepared_having.output_column_count ||
              node.output_descriptor_ids !=
                  prepared_having.row_binding.row_descriptor_ids) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-INPUT-V1";
            step.diagnostic.detail =
                "HAVING grouped COUNT/SUM input shape differs from the selected cost";
            return step;
          }

          const auto input_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  input_batch,
                  prepared_having.row_binding.row_descriptor_ids);
          if (!input_validation.ok) {
            step.diagnostic = input_validation;
            return step;
          }

          CanonicalRelationalExpressionRuntime expression_runtime(
              relational_dag, expression_services);
          std::vector<api::EngineSqlTruthValue> row_truth_values;
          row_truth_values.reserve(input_batch.rows.size());
          for (const auto& row : input_batch.rows) {
            api::EngineSqlTruthValue predicate_truth =
                api::EngineSqlTruthValue::unknown;
            std::string predicate_detail;
            if (!expression_runtime.EvaluatePredicateForConsumer(
                    prepared_having.predicate_expression_id,
                    prepared_having.row_binding, row.values,
                    api::EngineCanonicalExpressionConsumer::aggregate,
                    &predicate_truth, &predicate_detail)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-HAVING-INPUT-V1";
              step.diagnostic.detail =
                  "HAVING canonical predicate refused: " + predicate_detail;
              return step;
            }
            row_truth_values.push_back(predicate_truth);
          }

          exec::CanonicalDescriptorFilterRequest filter_request;
          filter_request.physical_dag = dag;
          filter_request.selected_physical_node_id = node.physical_node_id;
          filter_request.input_batch = input_batch;
          filter_request.row_truth_values = std::move(row_truth_values);
          filter_request.consumer = api::EnginePredicateConsumer::having;
          filter_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(mga_context, dag);
          const auto filter_result =
              exec::ExecuteCanonicalDescriptorFilter(filter_request);
          if (!filter_result.diagnostic.ok) {
            step.diagnostic = filter_result.diagnostic;
            return step;
          }
          step.result_handle_id = node.physical_node_id;
          step.input_row_count = input_batch.rows.size();
          step.rows_examined = input_batch.rows.size();
          step.output_row_count = filter_result.output_batch.rows.size();
          step.materialized_output_batch = filter_result.output_batch;
          step.mga_statement_context = filter_result.mga_statement_context;
          return step;
        };
  }

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(aggregate_registration));
  if (has_having) {
    execution_request.available_executors.push_back(
        std::move(having_registration));
  }
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          has_having ? "grouped-having.execution-attempt"
                     : "grouped-aggregate.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          has_having ? "grouped-having.transaction-effect-unchanged"
                     : "grouped-aggregate.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      maximum_output_rows;

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live grouped COUNT/SUM selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// QOW-SOURCE-QRY-019-PIVOT-LIVE-V1
// The bound carrier is item-major: group identifiers, one FOR identifier,
// one aggregate expression per (IN item, aggregate), then one fixed literal
// per IN item.  This keeps parser syntax out of execution while allowing the
// selected physical node to consume an arbitrary aggregate list.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreePivotQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kPivot ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const bool include_nulls =
      root->semantic_variant_id ==
      "pivot.fixed-aggregate-list-one-for.include-nulls.v1";
  const bool exclude_nulls =
      root->semantic_variant_id ==
      "pivot.fixed-aggregate-list-one-for.exclude-nulls.v1";
  if (!include_nulls && !exclude_nulls) return result;
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty()) return result;
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-ADMISSION-V1",
                  "PIVOT lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT input VALUES: " + input.detail);
  }
  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : request.relational_dag.outputs) {
    if (output.relation_node_id == root->logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, [](const auto* left, const auto* right) {
    return left->ordinal < right->ordinal;
  });
  const auto find_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        request.relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  if (outputs.size() != root->output_descriptor_ids.size() ||
      outputs.size() < 2 || root->bound_expression_ids.size() <=
                                outputs.size() + 1) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT output or bound-expression coverage is incomplete");
  }
  std::size_t group_count = 0;
  while (group_count < outputs.size()) {
    const auto expression = find_expression(outputs[group_count]->expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier) {
      break;
    }
    ++group_count;
  }
  const auto aggregate_output_count = outputs.size() - group_count;
  const auto item_count =
      root->bound_expression_ids.size() - outputs.size() - 1;
  if (group_count == 0 || item_count == 0 || aggregate_output_count == 0 ||
      aggregate_output_count % item_count != 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  "PIVOT group, aggregate, or fixed IN arity is unresolved");
  }
  const auto aggregate_count = aggregate_output_count / item_count;
  const auto map_identifier = [&](const std::uint32_t expression_id,
                                  std::size_t* column,
                                  std::string* detail) {
    const auto expression = find_expression(expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        expression->literal_or_parameter_ref.has_value()) {
      *detail = "PIVOT key is not an exact bound identifier";
      return false;
    }
    const auto descriptor = std::ranges::find(
        input_node->output_descriptor_ids, expression->result_descriptor_id);
    if (descriptor == input_node->output_descriptor_ids.end() ||
        std::ranges::count(input_node->output_descriptor_ids,
                           expression->result_descriptor_id) != 1) {
      *detail = "PIVOT identifier is not uniquely supplied by VALUES";
      return false;
    }
    *column = static_cast<std::size_t>(std::distance(
        input_node->output_descriptor_ids.begin(), descriptor));
    if (*column >= input.batch.columns.size() ||
        input.batch.columns[*column].descriptor_id !=
            expression->result_descriptor_id) {
      *detail = "PIVOT identifier ordinal is not descriptor-exact";
      return false;
    }
    return true;
  };

  std::vector<exec::CanonicalDescriptorOrderTerm> group_terms;
  std::vector<std::size_t> group_columns;
  std::string detail;
  for (std::size_t group = 0; group < group_count; ++group) {
    if (outputs[group]->ordinal != group || !outputs[group]->visible ||
        outputs[group]->descriptor_id != root->output_descriptor_ids[group] ||
        outputs[group]->expression_id != root->bound_expression_ids[group]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT group output lineage is not exact");
    }
    std::size_t column = 0;
    if (!map_identifier(root->bound_expression_ids[group], &column, &detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1", detail);
    }
    exec::CanonicalDescriptorOrderTerm term;
    term.column = column;
    term.expression_descriptor_id = input.batch.columns[column].descriptor_id;
    const auto validation = exec::ValidateCanonicalDescriptorOrderTerm(
        term, input.batch.columns[column]);
    if (!validation.ok) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    validation.diagnostic_code + ":" + validation.detail);
    }
    group_columns.push_back(column);
    group_terms.push_back(std::move(term));
  }
  std::size_t for_column = 0;
  if (!map_identifier(root->bound_expression_ids[group_count], &for_column,
                      &detail) ||
      std::ranges::find(group_columns, for_column) != group_columns.end()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  detail.empty() ? "PIVOT FOR key overlaps its group keys"
                                 : detail);
  }
  exec::CanonicalDescriptorOrderTerm for_term;
  for_term.column = for_column;
  for_term.expression_descriptor_id =
      input.batch.columns[for_column].descriptor_id;
  const auto for_validation = exec::ValidateCanonicalDescriptorOrderTerm(
      for_term, input.batch.columns[for_column]);
  if (!for_validation.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                  for_validation.diagnostic_code + ":" +
                      for_validation.detail);
  }

  const auto aggregate_expression_offset = group_count + 1;
  for (std::size_t output = 0; output < aggregate_output_count; ++output) {
    if (outputs[group_count + output]->ordinal != group_count + output ||
        !outputs[group_count + output]->visible ||
        outputs[group_count + output]->descriptor_id !=
            root->output_descriptor_ids[group_count + output] ||
        outputs[group_count + output]->expression_id !=
            root->bound_expression_ids[aggregate_expression_offset + output]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT aggregate output lineage is not exact");
    }
  }

  std::vector<std::vector<PreparedGlobalAggregateRoot>> prepared_aggregates(
      aggregate_count);
  for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
    const exec::CanonicalAggregateRegistryEntry* expected_registry = nullptr;
    for (std::size_t item = 0; item < item_count; ++item) {
      const auto output_ordinal =
          group_count + item * aggregate_count + aggregate;
      const auto expression_id =
          root->bound_expression_ids[aggregate_expression_offset +
                                     item * aggregate_count + aggregate];
      const auto expression = find_expression(expression_id);
      const auto* registry =
          expression == request.relational_dag.expressions.end() ||
                  !expression->function_uuid.has_value()
              ? nullptr
              : exec::LookupCanonicalAggregateByUuidV1(
                    *expression->function_uuid);
      if (registry == nullptr || !registry->executable ||
          (expected_registry != nullptr &&
           registry->function != expected_registry->function)) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                      "PIVOT aggregate identity differs across fixed IN items");
      }
      expected_registry = registry;
      const bool count_star =
          registry->function == exec::CanonicalAggregateFunction::count &&
          expression->child_expression_ids.empty();
      auto aggregate_root = *root;
      aggregate_root.output_descriptor_ids = {
          root->output_descriptor_ids[output_ordinal]};
      aggregate_root.bound_expression_ids = {expression_id};
      auto prepared = PrepareGlobalAggregateRoot(
          request.relational_dag, aggregate_root, *input_node, input,
          registry->function, count_star, false, false,
          static_cast<std::uint32_t>(output_ordinal), true);
      if (!prepared.ok) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                      prepared.detail);
      }
      if (!prepared_aggregates[aggregate].empty()) {
        const auto& first = prepared_aggregates[aggregate].front();
        if (prepared.aggregate_descriptor.function !=
                first.aggregate_descriptor.function ||
            prepared.aggregate_descriptor.count_star !=
                first.aggregate_descriptor.count_star ||
            prepared.value_columns != first.value_columns ||
            prepared.value_descriptor_ids != first.value_descriptor_ids ||
            prepared.distinct != first.distinct) {
          return refuse(
              "QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
              "PIVOT aggregate binding differs across fixed IN items");
        }
      }
      prepared_aggregates[aggregate].push_back(std::move(prepared));
    }
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::vector<exec::CanonicalPivotInItem> in_items;
  const auto literal_offset =
      aggregate_expression_offset + aggregate_output_count;
  for (std::size_t item = 0; item < item_count; ++item) {
    const auto expression_id = root->bound_expression_ids[literal_offset + item];
    const auto expression = find_expression(expression_id);
    api::EngineTypedValue value;
    std::string literal_detail;
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        !expression->child_expression_ids.empty() ||
        expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        !expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        !expression->literal_or_parameter_ref.has_value() ||
        !expression_runtime.EvaluateForConsumer(
            expression_id,
            input.batch.columns[for_column].descriptor.canonical_type_name,
            api::EngineCanonicalExpressionConsumer::aggregate, &value,
            &literal_detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT fixed IN item is not a canonical literal: " +
                        literal_detail);
    }
    in_items.push_back({{std::move(value)}});
  }

  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  result_columns.reserve(outputs.size());
  result_bindings.reserve(outputs.size());
  for (std::size_t group = 0; group < group_count; ++group) {
    auto column = input.batch.columns[group_columns[group]];
    column.stable_name = outputs[group]->output_name_utf8;
    column.descriptor_id = outputs[group]->descriptor_id;
    result_columns.push_back(std::move(column));
    auto binding = input.result_bindings[group_columns[group]];
    binding.physical_column_ordinal = group;
    if (!binding.published_descriptor.has_value()) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-PIVOT-PAYLOAD-V1",
                    "PIVOT group result descriptor is not publishable");
    }
    binding.published_descriptor->ordinal = static_cast<std::uint32_t>(group);
    binding.published_descriptor->name_utf8 = outputs[group]->output_name_utf8;
    result_bindings.push_back(std::move(binding));
  }
  for (std::size_t item = 0; item < item_count; ++item) {
    for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
      result_columns.push_back(
          prepared_aggregates[aggregate][item].result_column);
      result_bindings.push_back(
          prepared_aggregates[aggregate][item].result_bindings.front());
    }
  }
  std::vector<exec::CanonicalPivotAggregateBinding> aggregate_bindings;
  aggregate_bindings.reserve(aggregate_count);
  for (std::size_t aggregate = 0; aggregate < aggregate_count; ++aggregate) {
    const auto& prepared = prepared_aggregates[aggregate].front();
    exec::CanonicalPivotAggregateBinding binding;
    binding.aggregate_template.descriptor = prepared.aggregate_descriptor;
    binding.aggregate_template.value_columns = prepared.value_columns;
    binding.aggregate_template.value_expression_descriptor_ids =
        prepared.value_descriptor_ids;
    binding.aggregate_template.direct_arguments = prepared.direct_arguments;
    binding.aggregate_template.distinct = prepared.distinct;
    binding.aggregate_template.aggregate_order_terms =
        prepared.aggregate_order_terms;
    binding.aggregate_template.aggregate_separator =
        prepared.aggregate_separator;
    binding.aggregate_template.listagg_overflow_mode =
        prepared.listagg_overflow_mode;
    binding.aggregate_template.listagg_max_output_bytes =
        prepared.listagg_max_output_bytes;
    binding.aggregate_template.listagg_truncation_indicator =
        prepared.listagg_truncation_indicator;
    binding.aggregate_template.listagg_with_count = prepared.listagg_with_count;
    binding.aggregate_template.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::serial;
    for (std::size_t item = 0; item < item_count; ++item) {
      binding.result_columns_by_item.push_back(
          prepared_aggregates[aggregate][item].result_column);
    }
    aggregate_bindings.push_back(std::move(binding));
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t output_cells = 0;
  std::uint64_t output_memory = 0;
  std::uint64_t total_memory = 0;
  std::uint64_t group_comparisons = 0;
  std::uint64_t item_comparisons = 0;
  std::uint64_t maximum_key_comparisons = 0;
  std::uint64_t maximum_transitions = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(input_row_count, outputs.size(), &output_cells) ||
      !CheckedMultiply(output_cells, 64U, &output_memory) ||
      !CheckedAdd(input_memory, output_memory, &total_memory) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &group_comparisons) ||
      !CheckedMultiply(group_comparisons, group_count,
                       &group_comparisons) ||
      !CheckedMultiply(input_row_count, item_count, &item_comparisons) ||
      !CheckedAdd(group_comparisons, item_comparisons,
                  &maximum_key_comparisons) ||
      !CheckedMultiply(input_row_count, aggregate_count,
                       &maximum_transitions) ||
      total_memory > request.optimizer_request.resource.memory_budget_bytes ||
      maximum_key_comparisons > std::numeric_limits<std::size_t>::max() ||
      maximum_transitions > std::numeric_limits<std::size_t>::max() ||
      output_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "PIVOT live cost or resource bound is invalid");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto pivot_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "pivot.capability");
  const std::string pivot_implementation_id =
      include_nulls ? "pivot.canonical.include-nulls.typed.v1"
                    : "pivot.canonical.exclude-nulls.typed.v1";
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       pivot_implementation_id,
       pivot_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kPivot,
       exec::PhysicalNodeKind::kPivot,
       "canonical." + root->semantic_variant_id,
       input_row_count,
       std::max<std::uint64_t>(1, total_memory),
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "pivot.selected-plan", "PIVOT");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-PIVOT-VALUES-V1", "PIVOT");
  exec::CanonicalPhysicalExecutorRegistration pivot_registration;
  pivot_registration.node_kind = exec::PhysicalNodeKind::kPivot;
  pivot_registration.implementation_id = pivot_implementation_id;
  pivot_registration.executor_capability_uuid = pivot_capability_uuid;
  pivot_registration.executor_capability_abi_version = 1;
  pivot_registration.engine_owned = true;
  pivot_registration.accepts_optimizer_publication_v2 = true;
  pivot_registration.execute =
      [group_terms = std::move(group_terms), for_term = std::move(for_term),
       in_items = std::move(in_items),
       aggregate_bindings = std::move(aggregate_bindings),
       result_columns = std::move(result_columns), include_nulls,
       input_row_count,
       maximum_key_comparisons = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_key_comparisons)),
       maximum_transitions = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_transitions)),
       maximum_output_cells = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, output_cells)),
       maximum_state_bytes = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, total_memory)),
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() !=
                input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-PIVOT-INPUT-V1";
          step.diagnostic.detail =
              "PIVOT executor did not receive its exact selected input batch";
          return step;
        }
        auto bindings = aggregate_bindings;
        for (auto& binding : bindings) {
          binding.aggregate_template.maximum_transition_count =
              std::max<std::size_t>(1, input_row_count);
          binding.aggregate_template.maximum_state_bytes = maximum_state_bytes;
        }
        exec::CanonicalPivotRequest pivot_request;
        pivot_request.physical_dag = dag;
        pivot_request.selected_physical_node_id = node.physical_node_id;
        pivot_request.input_batch =
            *inputs.front().materialized_output_batch;
        pivot_request.group_key_terms = group_terms;
        pivot_request.for_key_terms = {for_term};
        pivot_request.in_items = in_items;
        pivot_request.aggregates = std::move(bindings);
        pivot_request.result_columns = result_columns;
        pivot_request.null_policy =
            include_nulls ? exec::CanonicalPivotNullPolicy::kInclude
                          : exec::CanonicalPivotNullPolicy::kExclude;
        pivot_request.maximum_key_comparison_count = maximum_key_comparisons;
        pivot_request.maximum_total_aggregate_transition_count =
            maximum_transitions;
        pivot_request.maximum_output_row_count =
            std::max<std::size_t>(1, input_row_count);
        pivot_request.maximum_output_cell_count = maximum_output_cells;
        pivot_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto pivot = exec::ExecuteCanonicalPivot(pivot_request);
        if (!pivot.diagnostic.ok) {
          step.diagnostic = pivot.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_row_count;
        step.rows_examined = input_row_count;
        step.output_row_count = pivot.output_batch.rows.size();
        step.materialized_output_batch = pivot.output_batch;
        step.mga_statement_context = pivot.mga_statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(pivot_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               request.context.current_monotonic_ns,
                           "pivot.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "pivot.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);
  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-PIVOT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "PIVOT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// QOW-SOURCE-QRY-019-UNPIVOT-LIVE-V1
// The bound carrier is item-major: group identifiers, every source value
// identifier for each IN item, then one fixed label literal per item.  The
// result output identifies the first item's label and value descriptors; all
// later items must cast through those exact engine-owned descriptors.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeUnpivotQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kUnpivot ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const bool include_nulls =
      root->semantic_variant_id ==
      "unpivot.fixed-value-list.include-nulls.v1";
  const bool exclude_nulls =
      root->semantic_variant_id ==
      "unpivot.fixed-value-list.exclude-nulls.v1";
  if (!include_nulls && !exclude_nulls) return result;
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty()) return result;
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-ADMISSION-V1",
                  "UNPIVOT lacks optimizer admission");
  }
  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT input VALUES: " + input.detail);
  }
  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : request.relational_dag.outputs) {
    if (output.relation_node_id == root->logical_node_id) {
      outputs.push_back(&output);
    }
  }
  std::ranges::sort(outputs, [](const auto* left, const auto* right) {
    return left->ordinal < right->ordinal;
  });
  const auto find_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        request.relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  if (outputs.size() != root->output_descriptor_ids.size() ||
      outputs.size() < 3 || root->bound_expression_ids.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT output or bound-expression coverage is incomplete");
  }
  std::size_t group_count = 0;
  while (group_count < outputs.size()) {
    const auto expression = find_expression(outputs[group_count]->expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier) {
      break;
    }
    ++group_count;
  }
  if (group_count == 0 || group_count + 1 >= outputs.size()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT group, label, or value output arity is unresolved");
  }
  const auto label_output_expression =
      find_expression(outputs[group_count]->expression_id);
  if (label_output_expression == request.relational_dag.expressions.end() ||
      label_output_expression->expression_kind !=
          api::RelationalExpressionKind::kLiteral) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label output is not a fixed literal");
  }
  const auto value_count = outputs.size() - group_count - 1;
  const auto bound_tail_count =
      root->bound_expression_ids.size() -
      std::min(group_count, root->bound_expression_ids.size());
  if (root->bound_expression_ids.size() <= group_count || value_count == 0 ||
      bound_tail_count % (value_count + 1) != 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT fixed IN item/value arity is unresolved");
  }
  const auto item_count = bound_tail_count / (value_count + 1);
  if (item_count == 0) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT requires at least one fixed IN item");
  }
  const auto source_offset = group_count;
  const auto label_offset = group_count + item_count * value_count;
  if (label_offset + item_count != root->bound_expression_ids.size() ||
      outputs[group_count]->expression_id !=
          root->bound_expression_ids[label_offset]) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label lineage is not item-major and exact");
  }

  const auto map_identifier = [&](const std::uint32_t expression_id,
                                  std::size_t* column,
                                  std::string* detail) {
    const auto expression = find_expression(expression_id);
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        expression->literal_or_parameter_ref.has_value()) {
      *detail = "UNPIVOT source is not an exact bound identifier";
      return false;
    }
    const auto descriptor = std::ranges::find(
        input_node->output_descriptor_ids, expression->result_descriptor_id);
    if (descriptor == input_node->output_descriptor_ids.end() ||
        std::ranges::count(input_node->output_descriptor_ids,
                           expression->result_descriptor_id) != 1) {
      *detail = "UNPIVOT identifier is not uniquely supplied by VALUES";
      return false;
    }
    *column = static_cast<std::size_t>(std::distance(
        input_node->output_descriptor_ids.begin(), descriptor));
    if (*column >= input.batch.columns.size() ||
        input.batch.columns[*column].descriptor_id !=
            expression->result_descriptor_id) {
      *detail = "UNPIVOT identifier ordinal is not descriptor-exact";
      return false;
    }
    return true;
  };

  std::vector<std::size_t> group_columns;
  std::string detail;
  for (std::size_t group = 0; group < group_count; ++group) {
    if (outputs[group]->ordinal != group || !outputs[group]->visible ||
        outputs[group]->descriptor_id != root->output_descriptor_ids[group] ||
        outputs[group]->expression_id != root->bound_expression_ids[group]) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT group output lineage is not exact");
    }
    std::size_t column = 0;
    if (!map_identifier(root->bound_expression_ids[group], &column, &detail)) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1", detail);
    }
    group_columns.push_back(column);
  }

  std::vector<exec::CanonicalUnpivotInItem> in_items(item_count);
  std::vector<std::size_t> first_item_columns;
  for (std::size_t item = 0; item < item_count; ++item) {
    for (std::size_t value = 0; value < value_count; ++value) {
      const auto expression_id =
          root->bound_expression_ids[source_offset + item * value_count + value];
      std::size_t column = 0;
      if (!map_identifier(expression_id, &column, &detail) ||
          std::ranges::find(group_columns, column) != group_columns.end()) {
        return refuse(
            "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
            detail.empty() ? "UNPIVOT source overlaps a group column" : detail);
      }
      if (item == 0) {
        if (outputs[group_count + 1 + value]->expression_id != expression_id ||
            outputs[group_count + 1 + value]->descriptor_id !=
                input.batch.columns[column].descriptor_id) {
          return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                        "UNPIVOT value output lineage is not exact");
        }
        first_item_columns.push_back(column);
      } else if (input.batch.columns[column].descriptor.canonical_type_name !=
                 input.batch.columns[first_item_columns[value]]
                     .descriptor.canonical_type_name) {
        return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                      "UNPIVOT item value types are not reconcilable");
      }
      in_items[item].source_columns.push_back(column);
    }
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::string label_type;
  if (!expression_runtime.InferType(root->bound_expression_ids[label_offset],
                                    std::nullopt, &label_type, &detail) ||
      label_type == "null") {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label type is unresolved: " + detail);
  }
  for (std::size_t item = 0; item < item_count; ++item) {
    const auto expression_id = root->bound_expression_ids[label_offset + item];
    const auto expression = find_expression(expression_id);
    api::EngineTypedValue label;
    std::string label_detail;
    if (expression == request.relational_dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        !expression->child_expression_ids.empty() ||
        expression->bound_name_uuid.has_value() ||
        expression->function_uuid.has_value() ||
        !expression->literal_kind.has_value() ||
        expression->operator_name.has_value() ||
        !expression->literal_or_parameter_ref.has_value() ||
        !expression_runtime.EvaluateForConsumer(
            expression_id, label_type,
            api::EngineCanonicalExpressionConsumer::projection, &label,
            &label_detail) ||
        label.state != api::EngineValueState::value || label.is_null) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT fixed label is invalid: " + label_detail);
    }
    in_items[item].pivot_value = std::move(label);
  }

  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  result_columns.reserve(outputs.size());
  for (std::size_t group = 0; group < group_count; ++group) {
    auto column = input.batch.columns[group_columns[group]];
    column.stable_name = outputs[group]->output_name_utf8;
    result_columns.push_back(std::move(column));
  }
  const auto label_descriptor = std::ranges::find_if(
      request.relational_dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id == outputs[group_count]->descriptor_id;
      });
  if (label_descriptor == request.relational_dag.descriptors.end() ||
      label_descriptor->descriptor_uuid !=
          in_items.front().pivot_value.descriptor.descriptor_uuid.canonical ||
      outputs[group_count]->output_name_utf8.empty()) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                  "UNPIVOT label result descriptor is unresolved");
  }
  result_columns.push_back(
      {outputs[group_count]->output_name_utf8,
       in_items.front().pivot_value.descriptor,
       label_descriptor->nullability == api::RelationalNullability::kNullable,
       label_descriptor->descriptor_id});
  for (std::size_t value = 0; value < value_count; ++value) {
    auto column = input.batch.columns[first_item_columns[value]];
    column.stable_name = outputs[group_count + 1 + value]->output_name_utf8;
    result_columns.push_back(std::move(column));
  }
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  result_bindings.reserve(outputs.size());
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = std::ranges::find_if(
        request.relational_dag.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == outputs[ordinal]->descriptor_id;
        });
    if (descriptor == request.relational_dag.descriptors.end() ||
        !outputs[ordinal]->visible || outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->output_name_utf8.empty()) {
      return refuse("QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-PAYLOAD-V1",
                    "UNPIVOT result binding is not publishable");
    }
    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(ordinal),
        outputs[ordinal]->output_name_utf8,
        descriptor->descriptor_uuid,
        descriptor->type_uuid,
        ResultNullability(descriptor->nullability),
        descriptor->collation_uuid,
        descriptor->timezone_profile_id};
    result_bindings.push_back(std::move(binding));
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t maximum_output_rows = 0;
  std::uint64_t maximum_output_cells = 0;
  std::uint64_t input_memory = 1;
  std::uint64_t output_memory = 0;
  std::uint64_t total_memory = 0;
  if (!CheckedMultiply(input_row_count, item_count, &maximum_output_rows) ||
      !CheckedMultiply(maximum_output_rows, outputs.size(),
                       &maximum_output_cells) ||
      !AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedMultiply(maximum_output_cells, 64U, &output_memory) ||
      !CheckedAdd(input_memory, output_memory, &total_memory) ||
      total_memory > request.optimizer_request.resource.memory_budget_bytes ||
      maximum_output_rows > std::numeric_limits<std::size_t>::max() ||
      maximum_output_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "UNPIVOT live cost or resource bound is invalid");
  }

  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto unpivot_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "unpivot.capability");
  const std::string unpivot_implementation_id =
      include_nulls ? "unpivot.canonical.include-nulls.typed.v1"
                    : "unpivot.canonical.exclude-nulls.typed.v1";
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       unpivot_implementation_id,
       unpivot_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kUnpivot,
       exec::PhysicalNodeKind::kUnpivot,
       "canonical." + root->semantic_variant_id,
       maximum_output_rows,
       std::max<std::uint64_t>(1, total_memory),
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "unpivot.selected-plan", "UNPIVOT");
  if (!planning.ok) return refuse(planning.diagnostic_id, planning.detail);
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-VALUES-V1", "UNPIVOT");
  exec::CanonicalPhysicalExecutorRegistration unpivot_registration;
  unpivot_registration.node_kind = exec::PhysicalNodeKind::kUnpivot;
  unpivot_registration.implementation_id = unpivot_implementation_id;
  unpivot_registration.executor_capability_uuid = unpivot_capability_uuid;
  unpivot_registration.executor_capability_abi_version = 1;
  unpivot_registration.engine_owned = true;
  unpivot_registration.accepts_optimizer_publication_v2 = true;
  unpivot_registration.execute =
      [group_columns = std::move(group_columns), in_items = std::move(in_items),
       result_columns = std::move(result_columns), include_nulls,
       input_row_count,
       maximum_output_rows = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_output_rows)),
       maximum_output_cells = static_cast<std::size_t>(
           std::max<std::uint64_t>(1, maximum_output_cells)),
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() !=
                input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-INPUT-V1";
          step.diagnostic.detail =
              "UNPIVOT executor did not receive its exact selected input batch";
          return step;
        }
        exec::CanonicalUnpivotRequest unpivot_request;
        unpivot_request.physical_dag = dag;
        unpivot_request.selected_physical_node_id = node.physical_node_id;
        unpivot_request.input_batch =
            *inputs.front().materialized_output_batch;
        unpivot_request.group_columns = group_columns;
        unpivot_request.in_items = in_items;
        unpivot_request.result_columns = result_columns;
        unpivot_request.null_policy =
            include_nulls ? exec::CanonicalPivotNullPolicy::kInclude
                          : exec::CanonicalPivotNullPolicy::kExclude;
        unpivot_request.maximum_output_row_count = maximum_output_rows;
        unpivot_request.maximum_output_cell_count = maximum_output_cells;
        unpivot_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto unpivot = exec::ExecuteCanonicalUnpivot(unpivot_request);
        if (!unpivot.diagnostic.ok) {
          step.diagnostic = unpivot.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_row_count;
        step.rows_examined = input_row_count;
        step.output_row_count = unpivot.output_batch.rows.size();
        step.materialized_output_batch = unpivot.output_batch;
        step.mga_statement_context = unpivot.mga_statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(unpivot_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               request.context.current_monotonic_ns,
                           "unpivot.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "unpivot.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(maximum_output_rows));
  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-UNPIVOT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "UNPIVOT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeGlobalAggregateQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto unary_aggregate_profile =
      MatchLiveUnaryAggregateExpressionProfile(root->semantic_variant_id);
  const bool count_star = unary_aggregate_profile.count_star;
  const bool sum_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::sum;
  const bool avg_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::avg;
  const bool min_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::min;
  const bool max_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::max;
  const bool bool_and_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::bool_and;
  const bool bool_or_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::bool_or;
  const bool every_expression =
      unary_aggregate_profile.matched &&
      unary_aggregate_profile.function ==
          exec::CanonicalAggregateFunction::every;
  const auto string_aggregate_profile =
      MatchLiveStringAggregateExpressionProfile(root->semantic_variant_id);
  const bool unordered_string_agg_expression =
      string_aggregate_profile.matched && !string_aggregate_profile.ordered;
  const bool ordered_string_agg_expression =
      string_aggregate_profile.matched && string_aggregate_profile.ordered;
  const bool string_agg_expression =
      unordered_string_agg_expression || ordered_string_agg_expression;
  const auto listagg_profile =
      MatchLiveListaggExpressionProfile(root->semantic_variant_id);
  const bool listagg_expression = listagg_profile.matched;
  const auto ordered_single_collection_profile =
      MatchLiveOrderedSingleCollectionExpressionProfile(
          root->semantic_variant_id);
  const bool array_agg_expression =
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function ==
          exec::CanonicalAggregateFunction::array_agg;
  const bool json_agg_expression =
      ordered_single_collection_profile.matched &&
      ordered_single_collection_profile.function ==
          exec::CanonicalAggregateFunction::json_agg;
  const auto json_object_aggregate_profile =
      MatchLiveJsonObjectAggregateExpressionProfile(
          root->semantic_variant_id);
  const bool json_object_agg_expression =
      json_object_aggregate_profile.matched;
  const bool ordered_single_collection_expression =
      array_agg_expression || json_agg_expression;
  const bool ordered_collection_expression =
      ordered_single_collection_expression || json_object_agg_expression;
  const bool statistical_expression =
      unary_aggregate_profile.matched &&
      (unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev_pop ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance_pop ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::stddev_samp ||
       unary_aggregate_profile.function ==
           exec::CanonicalAggregateFunction::variance_samp);
  const auto pair_statistical_profile =
      MatchLivePairStatisticalExpressionProfile(root->semantic_variant_id);
  const bool pair_statistical_expression = pair_statistical_profile.matched;
  const auto ordered_set_profile =
      MatchLiveOrderedSetExpressionProfile(root->semantic_variant_id);
  const bool ordered_set_expression = ordered_set_profile.matched;
  const auto approximate_profile =
      MatchLiveApproximateExpressionProfile(root->semantic_variant_id);
  const bool approximate_expression = approximate_profile.matched;
  if (!unary_aggregate_profile.matched &&
      !string_agg_expression && !listagg_expression &&
      !ordered_collection_expression &&
      !statistical_expression &&
      !pair_statistical_expression && !ordered_set_expression &&
      !approximate_expression) {
    return result;
  }
  auto aggregate_function = unary_aggregate_profile.matched
                                ? unary_aggregate_profile.function
                                : exec::CanonicalAggregateFunction::count;
  if (string_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::string_agg;
  }
  if (listagg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::listagg;
  }
  if (array_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::array_agg;
  }
  if (json_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_agg;
  }
  if (json_object_agg_expression) {
    aggregate_function = exec::CanonicalAggregateFunction::json_object_agg;
  }
  if (pair_statistical_expression) {
    aggregate_function = pair_statistical_profile.function;
  }
  if (ordered_set_expression) {
    aggregate_function = ordered_set_profile.function;
  }
  if (approximate_expression) {
    aggregate_function = approximate_profile.function;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-ADMISSION-V1",
                  "live global aggregate execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  "global aggregate input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareGlobalAggregateRoot(
      request.relational_dag, *root, *input_node, input, aggregate_function,
      count_star,
      unary_aggregate_profile.distinct || pair_statistical_profile.distinct ||
          string_aggregate_profile.distinct ||
          ordered_single_collection_profile.distinct ||
          json_object_aggregate_profile.distinct || listagg_profile.distinct ||
          ordered_set_profile.distinct || approximate_profile.distinct,
      unary_aggregate_profile.has_filter ||
          pair_statistical_profile.has_filter ||
          string_aggregate_profile.has_filter ||
          ordered_single_collection_profile.has_filter ||
          json_object_aggregate_profile.has_filter ||
          listagg_profile.has_filter || ordered_set_profile.has_filter ||
          approximate_profile.has_filter);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  constexpr std::uint64_t kIntegerAggregateResultMemory =
      std::numeric_limits<std::int64_t>::digits10 + 2;
  constexpr std::uint64_t kRealAggregateResultMemory = 64;
  constexpr std::uint64_t kBooleanAggregateResultMemory = 5;
  const bool pair_real_result =
      pair_statistical_expression &&
      aggregate_function != exec::CanonicalAggregateFunction::regr_count;
  const bool ordered_set_real_result =
      aggregate_function == exec::CanonicalAggregateFunction::percent_rank ||
      aggregate_function == exec::CanonicalAggregateFunction::cume_dist ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_cont ||
      aggregate_function == exec::CanonicalAggregateFunction::percentile_disc;
  const bool approximate_real_result =
      aggregate_function == exec::CanonicalAggregateFunction::approx_median ||
      aggregate_function ==
          exec::CanonicalAggregateFunction::approx_percentile_cont ||
      aggregate_function ==
          exec::CanonicalAggregateFunction::approx_percentile_disc;
  std::uint64_t aggregate_result_memory =
      (avg_expression || statistical_expression || pair_real_result ||
       ordered_set_real_result || approximate_real_result)
          ? kRealAggregateResultMemory
          : ((bool_and_expression || bool_or_expression || every_expression)
                 ? kBooleanAggregateResultMemory
                 : kIntegerAggregateResultMemory);
  if (!AddBatchMemoryBytes(input.batch, &input_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input size overflowed");
  }
  if (string_agg_expression || listagg_expression) {
    std::uint64_t separator_memory = 0;
    aggregate_result_memory = input_memory;
    if (!CheckedMultiply(
            static_cast<std::uint64_t>(input_row_count),
            static_cast<std::uint64_t>(
                prepared_root.aggregate_separator.size()),
            &separator_memory) ||
        !CheckedAdd(aggregate_result_memory, separator_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live STRING_AGG/LISTAGG result size overflowed");
    }
    if (ordered_string_agg_expression || listagg_expression) {
      std::uint64_t row_overhead_memory = 0;
      if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                           &row_overhead_memory) ||
          !CheckedAdd(aggregate_result_memory, row_overhead_memory,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live ordered STRING_AGG/LISTAGG state size overflowed");
      }
    }
  }
  if (prepared_root.filter_column.has_value()) {
    std::uint64_t filter_memory = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count),
                         sizeof(api::EngineSqlTruthValue), &filter_memory) ||
        !CheckedAdd(aggregate_result_memory, filter_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live aggregate FILTER state size overflowed");
    }
  }
  if (prepared_root.distinct) {
    std::uint64_t distinct_row_overhead = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &distinct_row_overhead) ||
        !CheckedAdd(aggregate_result_memory, input_memory,
                    &aggregate_result_memory) ||
        !CheckedAdd(aggregate_result_memory, distinct_row_overhead,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live aggregate DISTINCT state size overflowed");
    }
  }
  if (ordered_collection_expression) {
    std::uint64_t expanded_input_memory = 0;
    std::uint64_t row_overhead_memory = 0;
    const std::uint64_t input_expansion =
        (json_agg_expression || json_object_agg_expression) ? 6U : 1U;
    if (!CheckedMultiply(input_memory, input_expansion,
                         &expanded_input_memory) ||
        !CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &row_overhead_memory) ||
        !CheckedAdd(expanded_input_memory, row_overhead_memory,
                    &aggregate_result_memory) ||
        !CheckedAdd(aggregate_result_memory, 2U,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered collection aggregate result size "
                    "overflowed");
    }
  }
  if (ordered_set_expression) {
    std::uint64_t ordered_state_memory = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &ordered_state_memory) ||
        !CheckedAdd(aggregate_result_memory, ordered_state_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live ordered-set aggregate state size overflowed");
    }
  }
  if (approximate_expression) {
    std::uint64_t approximate_state_memory = 0;
    std::uint64_t row_overhead_memory = 0;
    if (!CheckedMultiply(static_cast<std::uint64_t>(input_row_count), 64U,
                         &row_overhead_memory) ||
        !CheckedAdd(input_memory, row_overhead_memory,
                    &approximate_state_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live approximate aggregate state size overflowed");
    }
    if (aggregate_function ==
        exec::CanonicalAggregateFunction::approx_top_k) {
      std::uint64_t rendered_value_memory = 0;
      if (!CheckedMultiply(input_memory, 6U, &rendered_value_memory) ||
          !CheckedAdd(rendered_value_memory, row_overhead_memory,
                      &rendered_value_memory) ||
          !CheckedAdd(rendered_value_memory, 2U,
                      &aggregate_result_memory)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live approximate top-k result size overflowed");
      }
    }
    if (!CheckedAdd(aggregate_result_memory, approximate_state_memory,
                    &aggregate_result_memory)) {
      return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                    "live approximate aggregate result size overflowed");
    }
  }
  if (!CheckedAdd(input_memory, aggregate_result_memory, &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live global aggregate input or result size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live global aggregate exceeds the admitted memory budget");
  }

  const std::string aggregate_implementation_id =
      count_star ? "aggregate.count-star.v1" : "aggregate.registry-core.v1";
  std::string aggregate_transformation_id;
  if (unary_aggregate_profile.matched) {
    aggregate_transformation_id = unary_aggregate_profile.transformation_id;
  } else if (string_agg_expression) {
    aggregate_transformation_id =
        string_aggregate_profile.transformation_id;
  } else if (listagg_expression) {
    aggregate_transformation_id = listagg_profile.transformation_id;
  } else if (array_agg_expression) {
    aggregate_transformation_id =
        ordered_single_collection_profile.transformation_id;
  } else if (json_agg_expression) {
    aggregate_transformation_id =
        ordered_single_collection_profile.transformation_id;
  } else if (json_object_agg_expression) {
    aggregate_transformation_id =
        json_object_aggregate_profile.transformation_id;
  } else if (pair_statistical_expression) {
    aggregate_transformation_id = pair_statistical_profile.transformation_id;
  } else if (ordered_set_expression) {
    aggregate_transformation_id = ordered_set_profile.transformation_id;
  } else if (approximate_expression) {
    aggregate_transformation_id = approximate_profile.transformation_id;
  } else {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-PAYLOAD-V1",
                  "live global aggregate transformation is unresolved");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto aggregate_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "aggregate.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       aggregate_implementation_id,
       aggregate_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       aggregate_transformation_id,
       input_row_count,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "aggregate.selected-plan", "AGGREGATE");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-VALUES-V1", "AGGREGATE");

  exec::CanonicalPhysicalExecutorRegistration aggregate_registration;
  aggregate_registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate_registration.implementation_id = aggregate_implementation_id;
  aggregate_registration.executor_capability_uuid =
      aggregate_capability_uuid;
  aggregate_registration.executor_capability_abi_version = 1;
  aggregate_registration.engine_owned = true;
  aggregate_registration.accepts_optimizer_publication_v2 = true;
  aggregate_registration.execute =
      [aggregate_descriptor = prepared_root.aggregate_descriptor,
       result_column = prepared_root.result_column,
       count_star = prepared_root.count_star,
       distinct = prepared_root.distinct,
       value_columns = prepared_root.value_columns,
       value_descriptor_ids = prepared_root.value_descriptor_ids,
       direct_arguments = prepared_root.direct_arguments,
       filter_column = prepared_root.filter_column,
       filter_descriptor_id = prepared_root.filter_descriptor_id,
       aggregate_order_terms = prepared_root.aggregate_order_terms,
       aggregate_separator = prepared_root.aggregate_separator,
       listagg_overflow_mode = prepared_root.listagg_overflow_mode,
       listagg_max_output_bytes = prepared_root.listagg_max_output_bytes,
       listagg_truncation_indicator =
           prepared_root.listagg_truncation_indicator,
       listagg_with_count = prepared_root.listagg_with_count,
       input_row_count, mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "global aggregate input cardinality differs from its selected cost";
          return step;
        }
        std::optional<std::vector<api::EngineSqlTruthValue>>
            filter_truth_values;
        if (filter_column.has_value()) {
          std::vector<api::EngineSqlTruthValue> materialized_filter;
          std::string filter_detail;
          if (!MaterializeAggregateFilterTruthValues(
                  input_batch, *filter_column, filter_descriptor_id,
                  &materialized_filter, &filter_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail = std::move(filter_detail);
            return step;
          }
          filter_truth_values = std::move(materialized_filter);
        }
        exec::DescriptorBatch output_batch;
        const auto mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        if (count_star) {
          exec::CanonicalDescriptorCountRequest aggregate_request;
          aggregate_request.physical_dag = dag;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.input_batch = input_batch;
          aggregate_request.count_column = result_column;
          aggregate_request.mga_authority = mga_authority;
          const auto aggregate_result =
              exec::ExecuteCanonicalDescriptorCountStar(aggregate_request);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = aggregate_result.diagnostic;
            return step;
          }
          output_batch = aggregate_result.output_batch;
        } else {
          exec::CanonicalAggregateRuntimeRequest aggregate_request;
          aggregate_request.physical_dag = dag;
          aggregate_request.selected_physical_node_id = node.physical_node_id;
          aggregate_request.descriptor = aggregate_descriptor;
          aggregate_request.input_batch = input_batch;
          aggregate_request.value_columns = value_columns;
          aggregate_request.value_expression_descriptor_ids =
              value_descriptor_ids;
          aggregate_request.direct_arguments = direct_arguments;
          aggregate_request.result_column = result_column;
          aggregate_request.filter_truth_values =
              std::move(filter_truth_values);
          aggregate_request.distinct = distinct;
          aggregate_request.aggregate_order_terms = aggregate_order_terms;
          aggregate_request.aggregate_separator = aggregate_separator;
          aggregate_request.listagg_overflow_mode = listagg_overflow_mode;
          aggregate_request.listagg_max_output_bytes =
              listagg_max_output_bytes;
          aggregate_request.listagg_truncation_indicator =
              listagg_truncation_indicator;
          aggregate_request.listagg_with_count = listagg_with_count;
          aggregate_request.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          aggregate_request.mga_authority = mga_authority;
          const auto aggregate_result =
              exec::ExecuteCanonicalAggregateRuntime(aggregate_request);
          if (!aggregate_result.diagnostic.ok) {
            step.diagnostic = aggregate_result.diagnostic;
            return step;
          }
          step.authority = aggregate_result.authority;
          output_batch = aggregate_result.output_batch;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = output_batch.rows.size();
        step.materialized_output_batch = std::move(output_batch);
        step.mga_statement_context = mga_authority.statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(aggregate_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "aggregate.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "aggregate.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count = 1;

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live global aggregate selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      root->semantic_variant_id != "limit.bound-count.v1" ||
      root->input_logical_node_ids.size() != 1 ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        !node.required_property_uuids.empty() ||
        !node.delivered_property_uuids.empty()) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-ADMISSION-V1",
                  "live LIMIT execution lacks optimizer admission");
  }
  if (root->bound_expression_ids.size() != 1) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "bound-count LIMIT requires exactly one expression");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT input VALUES: " + input.detail);
  }
  auto prepared_root = PrepareLimitRoot(request.relational_dag, *root,
                                        *input_node, input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  prepared_root.detail);
  }
  std::uint64_t row_limit = 0;
  std::string bound_detail;
  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  if (!EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids.front(),
          &row_limit, &bound_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-LIMIT-PAYLOAD-V1",
                  "LIMIT count: " + bound_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto output_row_bound =
      row_limit > input_row_count
          ? input_row_count
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedAdd(input_memory, output_row_bound == 0 ? 0 : input_memory,
                  &total_memory)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live LIMIT input or output size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live LIMIT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto limit_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "limit.capability");
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       "limit.typed.v1",
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       "canonical.limit.bound-count.v1",
       output_row_bound,
       total_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "limit.selected-plan", "LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-LIMIT-VALUES-V1", "LIMIT");

  exec::CanonicalPhysicalExecutorRegistration limit_registration;
  limit_registration.node_kind = exec::PhysicalNodeKind::kLimit;
  limit_registration.implementation_id = "limit.typed.v1";
  limit_registration.executor_capability_uuid = limit_capability_uuid;
  limit_registration.executor_capability_abi_version = 1;
  limit_registration.engine_owned = true;
  limit_registration.accepts_optimizer_publication_v2 = true;
  limit_registration.execute =
      [row_limit, input_row_count, mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-LIMIT-INPUT-V1";
          step.diagnostic.detail =
              "LIMIT input cardinality differs from its selected cost";
          return step;
        }
        exec::CanonicalDescriptorLimitRequest limit_request;
        limit_request.physical_dag = dag;
        limit_request.selected_physical_node_id = node.physical_node_id;
        limit_request.input_batch = input_batch;
        limit_request.limit = row_limit;
        limit_request.offset = 0;
        limit_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        const auto limit_result =
            exec::ExecuteCanonicalDescriptorLimit(limit_request);
        if (!limit_result.diagnostic.ok) {
          step.diagnostic = limit_result.diagnostic;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = limit_result.output_batch.rows.size();
        step.output_row_count = limit_result.output_batch.rows.size();
        step.materialized_output_batch = limit_result.output_batch;
        step.mga_statement_context = limit_result.mga_statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "limit.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-LIMIT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live LIMIT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeSortQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 2 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kSort ||
      root->semantic_variant_id != "sort.required-order.v1" ||
      root->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto input_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (input_node == graph.nodes.end() || input_node == root ||
      input_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      input_node->semantic_variant_id != "values.literal-table.v1" ||
      !input_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty() ||
        (node.logical_node_id == input_node->logical_node_id &&
         (!node.required_property_uuids.empty() ||
          !node.delivered_property_uuids.empty()))) {
      return result;
    }
  }
  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-ADMISSION-V1",
                  "live SORT execution lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *input_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  "SORT input VALUES: " + input.detail);
  }
  const bool expression_ordering = std::ranges::any_of(
      root->bound_expression_ids, [&](const auto expression_id) {
        return std::ranges::find(input_node->bound_expression_ids,
                                 expression_id) ==
               input_node->bound_expression_ids.end();
      });
  std::uint64_t expression_work = 0;
  if (expression_ordering &&
      (!CheckedMultiply(input.batch.rows.size(),
                        root->bound_expression_ids.size(),
                        &expression_work) ||
       expression_work >
           request.optimizer_request.resource.maximum_candidate_count)) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live expression SORT row evaluation exceeds the admitted "
                  "candidate bound");
  }
  auto prepared_root = expression_ordering
      ? PrepareExpressionSortRoot(
            request.context, request.relational_dag,
            request.optimizer_request.logical_properties, *root, *input_node,
            input, request.expression_services)
      : PrepareSortRoot(
            request.context, request.relational_dag,
            request.optimizer_request.logical_properties, *root, *input_node,
            input);
  if (!prepared_root.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-SORT-PAYLOAD-V1",
                  prepared_root.detail);
  }

  const auto input_row_count = input.batch.rows.size();
  std::uint64_t input_memory = 1;
  std::uint64_t expression_memory = 1;
  std::uint64_t comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t total_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      (prepared_root.expression_ordering &&
       !AddBatchMemoryBytes(prepared_root.expression_input_batch,
                            &expression_memory)) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &comparison_count) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(input_memory, input_memory, &total_memory) ||
      (prepared_root.expression_ordering &&
       (!CheckedAdd(total_memory, expression_memory, &total_memory) ||
        !CheckedAdd(total_memory, expression_memory, &total_memory))) ||
      !CheckedAdd(total_memory, comparison_count, &total_memory) ||
      !CheckedAdd(total_memory, row_order_memory, &total_memory) ||
      comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "live SORT comparison or materialization size overflowed");
  }
  if (total_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live SORT exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_root.ordering_property_uuid,
      "sort.deterministic-tie");
  const std::string sort_implementation_id =
      prepared_root.expression_ordering
          ? "sort.typed.expression-row.v1"
          : "sort.typed.terms.v1";
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {input_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {root->logical_node_id,
       sort_implementation_id,
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       prepared_root.expression_ordering
           ? "canonical.sort.expression-row.v1"
           : "canonical.sort.typed.terms.v1",
       input_row_count,
       total_memory,
       1,
       1,
       {},
       {prepared_root.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles, "sort.selected-plan", "SORT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(input_node->logical_node_id, std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-SORT-VALUES-V1", "SORT");

  exec::CanonicalPhysicalExecutorRegistration sort_registration;
  sort_registration.node_kind = exec::PhysicalNodeKind::kSort;
  sort_registration.implementation_id = sort_implementation_id;
  sort_registration.executor_capability_uuid = sort_capability_uuid;
  sort_registration.executor_capability_abi_version = 1;
  sort_registration.engine_owned = true;
  sort_registration.accepts_optimizer_publication_v2 = true;
  sort_registration.execute =
      [order_terms = prepared_root.order_terms,
       expression_ordering = prepared_root.expression_ordering,
       expressions = prepared_root.expressions,
       relational_dag = request.relational_dag,
       expression_services = request.expression_services,
       deterministic_tie_evidence_uuid, input_row_count,
       maximum_pair_comparisons =
           std::max<std::size_t>(1, static_cast<std::size_t>(comparison_count)),
       mga_context = request.context](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT executor did not receive one typed input batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        if (input_batch.rows.size() != input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "SORT input cardinality differs from its selected cost";
          return step;
        }
        const auto mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        exec::DescriptorBatch expression_batch;
        if (expression_ordering) {
          const auto input_validation = exec::ValidateCanonicalDescriptorBatch(
              input_batch, inputs.front().output_descriptor_ids);
          if (!input_validation.ok) {
            step.diagnostic = input_validation;
            return step;
          }
          const auto before =
              exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                              dag);
          if (!before.ok) {
            step.diagnostic = before;
            return step;
          }
          std::string expression_detail;
          if (!MaterializeExpressionSortBatch(
                  relational_dag, expressions, input_batch,
                  expression_services, &expression_batch,
                  &expression_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SORT-EXPRESSION-V1";
            step.diagnostic.detail = std::move(expression_detail);
            return step;
          }
        }

        exec::CanonicalDescriptorSortRequest sort_request;
        sort_request.physical_dag = dag;
        sort_request.selected_physical_node_id = node.physical_node_id;
        sort_request.input_batch = input_batch;
        if (expression_ordering) {
          sort_request.order_key_batch = std::move(expression_batch);
        }
        sort_request.order_terms = order_terms;
        sort_request.deterministic_tie_evidence_uuid =
            deterministic_tie_evidence_uuid;
        sort_request.maximum_pair_comparisons = maximum_pair_comparisons;
        sort_request.mga_authority = mga_authority;
        const auto sort_result =
            exec::ExecuteCanonicalDescriptorSort(sort_request);
        if (!sort_result.diagnostic.ok) {
          step.diagnostic = sort_result.diagnostic;
          return step;
        }
        exec::DescriptorBatch output_batch = sort_result.output_batch;
        if (expression_ordering) {
          const auto output_validation =
              exec::ValidateCanonicalDescriptorBatch(
                  output_batch, node.output_descriptor_ids);
          if (!output_validation.ok) {
            step.diagnostic = output_validation;
            return step;
          }
          const auto after =
              exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority,
                                                              dag);
          if (!after.ok) {
            step.diagnostic = after;
            return step;
          }
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = output_batch.rows.size();
        step.materialized_output_batch = std::move(output_batch);
        step.mga_statement_context = sort_result.mga_statement_context;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "sort.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "sort.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_root.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, input_row_count);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-SORT-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "live SORT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// QOW-SOURCE-RCP-020-ORDER-LIMIT-DISTINCT-COMPOSITION-V1
// Execute the canonical SQL evaluation tail as one selected physical DAG:
// projected VALUES -> query DISTINCT -> ORDER BY -> OFFSET plus either LIMIT
// or the one signed native FETCH FIRST ROWS ONLY profile. TOP and WITH TIES
// remain the exact QRY-010 profile refusals and are not promoted here.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeDistinctSortLimitQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  const auto root = std::ranges::find_if(graph.nodes, [&](const auto& node) {
    return node.logical_node_id == graph.root_logical_node_id;
  });
  if (graph.nodes.size() != 4 || root == graph.nodes.end() ||
      root->node_kind != plan::CanonicalLogicalRelationalNodeKind::kLimit ||
      (root->semantic_variant_id != "limit.bound-count-offset.v1" &&
       root->semantic_variant_id !=
           "fetch.first-rows-only-offset.v1") ||
      root->input_logical_node_ids.size() != 1 ||
      root->bound_expression_ids.size() != 2) {
    return result;
  }
  const bool fetch_first_rows_only =
      root->semantic_variant_id == "fetch.first-rows-only-offset.v1";
  const auto sort_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id == root->input_logical_node_ids.front();
      });
  if (sort_node == graph.nodes.end() || sort_node == root ||
      sort_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kSort ||
      sort_node->semantic_variant_id != "sort.required-order.v1" ||
      sort_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto distinct_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               sort_node->input_logical_node_ids.front();
      });
  if (distinct_node == graph.nodes.end() || distinct_node == root ||
      distinct_node == sort_node ||
      distinct_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kAggregate ||
      distinct_node->semantic_variant_id != "aggregate.query-distinct.v1" ||
      distinct_node->input_logical_node_ids.size() != 1) {
    return result;
  }
  const auto values_node =
      std::ranges::find_if(graph.nodes, [&](const auto& node) {
        return node.logical_node_id ==
               distinct_node->input_logical_node_ids.front();
      });
  if (values_node == graph.nodes.end() || values_node == root ||
      values_node == sort_node || values_node == distinct_node ||
      values_node->node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      values_node->semantic_variant_id != "values.literal-table.v1" ||
      !values_node->input_logical_node_ids.empty()) {
    return result;
  }
  for (const auto& node : graph.nodes) {
    if (!node.required_object_uuids.empty()) return result;
  }

  result.profile_matched = true;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result =
        Failure(request, std::move(diagnostic_id), std::move(detail));
    return result;
  };
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-ADMISSION-V1",
                  "DISTINCT/ORDER/LIMIT composition lacks optimizer admission");
  }

  auto input = MaterializeValues(request.relational_dag, *values_node,
                                 request.expression_services);
  if (!input.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  "composition input VALUES: " + input.detail);
  }
  auto prepared_distinct = PrepareQueryDistinctRoot(
      request.context, request.relational_dag, *distinct_node, *values_node,
      input);
  if (!prepared_distinct.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_distinct.detail);
  }
  auto prepared_sort = PrepareSortRoot(
      request.context, request.relational_dag,
      request.optimizer_request.logical_properties, *sort_node,
      *distinct_node, input);
  if (!prepared_sort.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_sort.detail);
  }
  auto prepared_limit = PrepareLimitRoot(
      request.relational_dag, *root, *sort_node, input);
  if (!prepared_limit.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  prepared_limit.detail);
  }

  CanonicalRelationalExpressionRuntime expression_runtime(
      request.relational_dag, request.expression_services);
  std::uint64_t row_limit = 0;
  std::uint64_t row_offset = 0;
  std::string bound_detail;
  if (!EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids[0], &row_limit,
          &bound_detail) ||
      !EvaluateNonNegativeRowBound(
          &expression_runtime, root->bound_expression_ids[1], &row_offset,
          &bound_detail)) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-PAYLOAD-V1",
                  "LIMIT/FETCH bound: " + bound_detail);
  }

  const auto input_row_count = input.batch.rows.size();
  const auto offset_bound =
      row_offset > input_row_count
          ? input_row_count
          : static_cast<std::size_t>(row_offset);
  const auto remaining_bound = input_row_count - offset_bound;
  const auto output_row_bound =
      row_limit > remaining_bound
          ? remaining_bound
          : static_cast<std::size_t>(row_limit);
  std::uint64_t input_memory = 1;
  std::uint64_t distinct_memory = 0;
  std::uint64_t distinct_comparison_count = 0;
  std::uint64_t distinct_self_comparison_count = 0;
  std::uint64_t sort_comparison_count = 0;
  std::uint64_t row_order_memory = 0;
  std::uint64_t sort_memory = 0;
  std::uint64_t limit_memory = 0;
  if (!AddBatchMemoryBytes(input.batch, &input_memory) ||
      !CheckedAdd(input_memory, input_memory, &distinct_memory) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &distinct_comparison_count) ||
      !CheckedMultiply(distinct_comparison_count,
                       input.batch.columns.size(),
                       &distinct_comparison_count) ||
      !CheckedMultiply(input_row_count, input.batch.columns.size(),
                       &distinct_self_comparison_count) ||
      !CheckedAdd(distinct_comparison_count,
                  distinct_self_comparison_count,
                  &distinct_comparison_count) ||
      !CheckedMultiply(input_row_count, input_row_count,
                       &sort_comparison_count) ||
      !CheckedMultiply(input_row_count, sizeof(std::size_t),
                       &row_order_memory) ||
      !CheckedAdd(distinct_memory, input_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, sort_comparison_count, &sort_memory) ||
      !CheckedAdd(sort_memory, row_order_memory, &sort_memory) ||
      !CheckedAdd(sort_memory, input_memory, &limit_memory) ||
      distinct_comparison_count > std::numeric_limits<std::size_t>::max() ||
      sort_comparison_count > std::numeric_limits<std::size_t>::max()) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                  "composition comparison or materialization size overflowed");
  }
  if (limit_memory >
      request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "composition exceeds the admitted memory budget");
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" +
      request.context.statement_uuid.canonical;
  const auto values_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto distinct_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "query-distinct.capability");
  const auto sort_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "sort.capability");
  const auto limit_capability_uuid = DerivedCanonicalUuid(
      identity_scope,
      fetch_first_rows_only ? "fetch.capability" : "limit.capability");
  const auto deterministic_tie_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + prepared_sort.ordering_property_uuid,
      "sort.deterministic-tie");
  const std::string limit_implementation_id =
      fetch_first_rows_only ? "fetch.native.rows-only.v1"
                            : "limit.typed.v1";
  const std::vector<LivePhysicalNodeProfile> profiles = {
      {values_node->logical_node_id,
       std::string(kValuesImplementationId),
       values_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kValues,
       exec::PhysicalNodeKind::kValues,
       "canonical.values.materialize.v1",
       input_row_count,
       input_memory,
       0,
       0},
      {distinct_node->logical_node_id,
       "aggregate.query-distinct.typed.v1",
       distinct_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kAggregate,
       exec::PhysicalNodeKind::kAggregate,
       "canonical.aggregate.query-distinct.v1",
       input_row_count,
       distinct_memory,
       1,
       1},
      {sort_node->logical_node_id,
       "sort.typed.terms.v1",
       sort_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kSort,
       exec::PhysicalNodeKind::kSort,
       "canonical.sort.typed.terms.v1",
       input_row_count,
       sort_memory,
       1,
       1,
       {},
       {prepared_sort.ordering_property_uuid},
       {plan::CanonicalLogicalPropertyKind::kOrdering}},
      {root->logical_node_id,
       limit_implementation_id,
       limit_capability_uuid,
       plan::CanonicalLogicalRelationalNodeKind::kLimit,
       exec::PhysicalNodeKind::kLimit,
       fetch_first_rows_only
           ? "canonical.fetch.first-rows-only-offset.v1"
           : "canonical.limit.bound-count-offset.v1",
       output_row_bound,
       limit_memory,
       1,
       1}};
  const auto planning = PlanAndPublishLivePhysicalDag(
      request, profiles,
      fetch_first_rows_only ? "fetch-distinct-sort.selected-plan"
                            : "limit-distinct-sort.selected-plan",
      "DISTINCT/ORDER/LIMIT");
  if (!planning.ok) {
    return refuse(planning.diagnostic_id, planning.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = planning.physical_dag.nodes.size();
  result.selected_plan_uuid = planning.physical_dag.selected_plan_uuid;

  std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
  values_batches.emplace(values_node->logical_node_id,
                          std::move(input.batch));
  auto values_registration = MakeLiveValuesRegistration(
      std::move(values_batches), values_capability_uuid,
      "QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-VALUES-V1",
      "DISTINCT/ORDER/LIMIT");
  auto distinct_registration = MakeLiveQueryDistinctRegistration(
      std::move(prepared_distinct.equality_terms),
      distinct_capability_uuid, input_row_count,
      std::max<std::size_t>(
          1, static_cast<std::size_t>(distinct_comparison_count)),
      request.context);
  auto sort_registration = MakeLiveSortRegistration(
      std::move(prepared_sort.order_terms),
      deterministic_tie_evidence_uuid, sort_capability_uuid,
      input_row_count,
      std::max<std::size_t>(1,
                            static_cast<std::size_t>(sort_comparison_count)),
      request.context);
  auto limit_registration = MakeLiveLimitRegistration(
      limit_implementation_id, limit_capability_uuid, row_limit, row_offset,
      fetch_first_rows_only, input_row_count, request.context);

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = planning.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      planning.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority = BuildCanonicalExecutionMgaAuthority(
      request.context, planning.physical_dag);
  execution_request.available_executors.push_back(
      std::move(values_registration));
  execution_request.available_executors.push_back(
      std::move(distinct_registration));
  execution_request.available_executors.push_back(
      std::move(sort_registration));
  execution_request.available_executors.push_back(
      std::move(limit_registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "distinct-sort-limit.execution-attempt");
  execution_request.result_publication_request
      .transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(request.context.local_transaction_id) + ":" +
          std::to_string(
              request.context.snapshot_visible_through_local_transaction_id),
      "distinct-sort-limit.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(prepared_limit.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, output_row_bound);

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-RELATIONAL-LIVE-COMPOSITION-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "DISTINCT/ORDER/LIMIT selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
void ArmCanonicalQueryPreResultRevocationForContractTest() {
  g_contract_revalidation_resolution_count = 0;
  g_contract_pre_result_revocation_armed = true;
}

void ArmCanonicalQuerySecurityBoundaryDriftForContractTest() {
  g_contract_security_boundary_drift_armed = true;
}

void ArmCanonicalQueryResourceBoundaryDriftForContractTest() {
  g_contract_resource_boundary_drift_armed = true;
}

std::size_t CanonicalQueryContractRevalidationCountForTest() {
  return g_contract_revalidation_resolution_count;
}
#endif

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& input_request) {
  auto request = input_request;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  // The textual closure seam does not own a durable transaction inventory.
  // Complete its already-bound statement vector here so the same ABI-v2
  // publication and executor revalidation used by production is exercised.
  auto closure_mga = request.optimizer_request.logical_graph
                         .mga_statement_context;
  closure_mga.oldest_active_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.oldest_interesting_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.oldest_snapshot_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.retention_horizon_transaction_id =
      closure_mga.owning_local_transaction_id;
  closure_mga.active_excluded_local_transaction_ids = {
      closure_mga.owning_local_transaction_id};
  closure_mga.in_doubt_excluded_local_transaction_ids.clear();
  closure_mga.snapshot_kind = "statement_stable";
  closure_mga.publication_inventory_next_local_transaction_id =
      std::max(closure_mga.owning_local_transaction_id,
               closure_mga.visible_committed_high_watermark) +
      1;
  closure_mga.inventory_authoritative = true;
  closure_mga.complete = true;
  closure_mga.current = true;
  request.optimizer_request.logical_graph.mga_statement_context = closure_mga;
  request.optimizer_request.logical_properties.mga_statement_context =
      closure_mga;
  request.optimizer_request.mga.statement_context = closure_mga;
  request.optimizer_admission.mga_statement_context = closure_mga;
#endif
  auto pivot = ExecuteCanonicalObjectFreePivotQuery(request);
  if (pivot.profile_matched) return pivot;
  auto unpivot = ExecuteCanonicalObjectFreeUnpivotQuery(request);
  if (unpivot.profile_matched) return unpivot;
  auto grouped_aggregate =
      ExecuteCanonicalObjectFreeGroupedCountSumQuery(request);
  if (grouped_aggregate.profile_matched) return grouped_aggregate;
  auto aggregate = ExecuteCanonicalObjectFreeGlobalAggregateQuery(request);
  if (aggregate.profile_matched) return aggregate;
  auto composition =
      ExecuteCanonicalObjectFreeDistinctSortLimitQuery(request);
  if (composition.profile_matched) return composition;
  auto full_filtered_tail =
      ExecuteCanonicalObjectFreeFilterProjectDistinctSortLimitQuery(request);
  if (full_filtered_tail.profile_matched) return full_filtered_tail;
  auto filter_project_sort_limit =
      ExecuteCanonicalObjectFreeFilterProjectSortLimitQuery(request);
  if (filter_project_sort_limit.profile_matched) {
    return filter_project_sort_limit;
  }
  auto filter_project_sort =
      ExecuteCanonicalObjectFreeFilterProjectSortQuery(request);
  if (filter_project_sort.profile_matched) return filter_project_sort;
  auto filter_project = ExecuteCanonicalObjectFreeFilterProjectQuery(request);
  if (filter_project.profile_matched) return filter_project;
  auto project_sort = ExecuteCanonicalObjectFreeProjectSortQuery(request);
  if (project_sort.profile_matched) return project_sort;
  auto sort = ExecuteCanonicalObjectFreeSortQuery(request);
  if (sort.profile_matched) return sort;
  auto limit = ExecuteCanonicalObjectFreeLimitQuery(request);
  if (limit.profile_matched) return limit;
  auto project = ExecuteCanonicalObjectFreeProjectQuery(request);
  if (project.profile_matched) return project;
  auto filter = ExecuteCanonicalObjectFreeFilterQuery(request);
  if (filter.profile_matched) return filter;
  auto join_filter_project =
      ExecuteCanonicalObjectFreeInnerJoinFilterProjectQuery(request);
  if (join_filter_project.profile_matched) return join_filter_project;
  auto join = ExecuteCanonicalObjectFreeJoinQuery(request);
  if (join.profile_matched) return join;
  auto nested_set_operation =
      ExecuteCanonicalObjectFreeNestedSetOperationQuery(request);
  if (nested_set_operation.profile_matched) return nested_set_operation;
  auto set_operation = ExecuteCanonicalObjectFreeSetOperationQuery(request);
  if (set_operation.profile_matched) return set_operation;
  auto node_composition =
      ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(request);
  if (node_composition.profile_matched) return node_composition;
  CanonicalObjectFreeValuesExecutionResult result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() != 1 ||
      graph.root_logical_node_id != graph.nodes.front().logical_node_id ||
      graph.nodes.front().node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      graph.nodes.front().semantic_variant_id != "values.literal-table.v1" ||
      !graph.nodes.front().input_logical_node_ids.empty() ||
      !graph.nodes.front().required_object_uuids.empty() ||
      !graph.nodes.front().required_property_uuids.empty() ||
      !graph.nodes.front().delivered_property_uuids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  result.profile_matched = true;
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-ADMISSION-V1",
                  "live VALUES execution lacks optimizer admission");
  }

  auto materialized = MaterializeValues(request.relational_dag,
                                        graph.nodes.front(),
                                        request.expression_services);
  if (!materialized.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1",
                  materialized.detail);
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "values.alternative");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto transformation_uuid =
      DerivedCanonicalUuid(identity_scope, "values.transformation");
  const auto cost_vector_uuid =
      DerivedCanonicalUuid(identity_scope, "values.cost-vector");
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "values.calibration");

  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;
  alternatives.mga_statement_context = graph.mga_statement_context;
  alternatives.alternatives.push_back(
      {alternative_uuid,
       graph.nodes.front().logical_node_id,
       std::string(kValuesImplementationId),
       capability_uuid,
       graph.nodes.front().output_descriptor_ids,
       true,
       {},
       {},
       {}});

  std::uint64_t memory_bytes = 1;
  for (const auto& row : materialized.batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - memory_bytes) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live VALUES materialization size overflowed");
      }
      memory_bytes += value.encoded_value.size();
    }
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live VALUES materialization exceeds the admitted memory budget");
  }

  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = alternative_uuid;
  candidate.transformation_uuid = transformation_uuid;
  candidate.transformation_rule_id = "canonical.values.materialize.v1";
  candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  candidate.statistics_snapshot_uuid =
      request.optimizer_admission.statistics_snapshot_uuid;
  candidate.statistics_generation =
      request.optimizer_admission.statistics_generation;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = cost_vector_uuid;
  candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
  candidate.cost_terms.cpu_units = materialized.batch.rows.size();
  candidate.cost_terms.memory_bytes_required = memory_bytes;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      {candidate}, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    const auto diagnostic = search.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                                : search.issues.front().diagnostic_id;
    const auto detail = search.issues.empty()
                            ? "live VALUES search returned no selected plan"
                            : search.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.optimizer_selected = true;

  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = capability_uuid;
  capability.capability_abi_version = 1;
  capability.implementation_id = kValuesImplementationId;
  capability.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  capability.physical_node_kind = exec::PhysicalNodeKind::kValues;
  capability.maximum_memory_bytes =
      request.optimizer_request.resource.memory_budget_bytes;
  capability.spill_supported = false;
  capability.available = true;
  capability.engine_owned = true;
  capabilities.capabilities.push_back(std::move(capability));

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, "values.selected-plan");
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    const auto diagnostic = publication.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                                : publication.issues.front().diagnostic_id;
    const auto detail = publication.issues.empty()
                            ? "live VALUES physical DAG was not published"
                            : publication.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_published = true;
  result.physical_node_count = publication.physical_dag.nodes.size();
  result.selected_plan_uuid = publication.physical_dag.selected_plan_uuid;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = kValuesImplementationId;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batch = materialized.batch](const exec::TypedPhysicalNodeDag& dag,
                                   const exec::PhysicalNodeRecord& node,
                                   const std::vector<
                                       exec::CanonicalPhysicalDispatchInput>&
                                       inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-INPUT-V1";
          step.diagnostic.detail = "VALUES executor received an input edge";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch.rows.size();
        step.rows_examined = batch.rows.size();
        step.materialized_output_batch = batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = publication.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          publication.physical_dag);
  execution_request.available_executors.push_back(std::move(registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "values.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "values.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(materialized.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, materialized.batch.rows.size());

  const auto execution =
      ExecuteSelectedWithMgaGuard(request.context, execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    const auto diagnostic = execution.issues.empty()
                                ? "QOW-DIAG-RELATIONAL-LIVE-VALUES-EXECUTION-V1"
                                : execution.issues.front().diagnostic_id;
    const auto detail = execution.issues.empty()
                            ? "live VALUES selected DAG was not completed"
                            : execution.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published =
      execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

// QOW-SOURCE-PACKET7-OBJECT-BACKED-HEAP-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalCurrentHeapQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  (void)input;
  return result;
#else
  const auto& dag = input.relational_dag;
  if (dag.wire_version != 2 || dag.nodes.size() != 1 ||
      dag.root_node_id != dag.nodes.front().node_id ||
      dag.nodes.front().node_kind != api::RelationalDagNodeKind::kScan ||
      dag.nodes.front().semantic_variant_id != "relation.source.v1" ||
      !dag.nodes.front().input_node_ids.empty() ||
      dag.nodes.front().required_object_uuids.size() != 1) {
    return result;
  }
  result.profile_matched = true;

  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = input.relational_dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto admission = api::BuildCanonicalCurrentHeapOptimizerAdmission(
      {input.context, input.relational_dag});
  if (!admission.built || !admission.admission.admitted ||
      !admission.admission.planning_allowed ||
      admission.admission.data_access_allowed) {
    return refuse(
        admission.issue.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-ADMISSION-V1"
            : admission.issue.diagnostic_id,
        admission.issue.field_id.empty()
            ? "current object-backed heap optimizer admission failed"
            : admission.issue.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_degraded =
      admission.admission.degraded_for_unknown_statistics;
  result.optimizer_benchmark_clean_ready =
      admission.admission.benchmark_clean_ready;
  result.optimizer_admission_stage_count =
      admission.admission.evidence.size();

  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, input.relational_dag, admission.request,
      admission.admission};
  const auto& graph = admission.request.logical_graph;
  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = graph.root_logical_node_id;
  profile.implementation_id = "scan.heap.v1";
  profile.capability_uuid =
      DerivedCanonicalUuid(identity_scope, "heap-scan.capability");
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  profile.transformation_rule_id = "canonical.heap.scan.v1";
  profile.estimated_rows = 1;
  profile.memory_bytes_required = 1024;
  profile.minimum_input_count = 0;
  profile.maximum_input_count = 0;
  profile.page_read_sequential_units = 1;
  profile.mga_visibility_checks_expected = 1;
  profile.storage_read_capable = true;
  profile.mga_visibility_capable = true;
  const auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, {profile}, "heap-scan.selected-plan",
      "object-backed heap scan");
  if (!physical.ok) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-PLANNING-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "object-backed heap physical DAG was not published"
            : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto bounded_size = [](const std::uint64_t value) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::size_t>::max()));
  };
  const std::size_t maximum_scanned_row_versions =
      bounded_size(std::min(input.context.optimizer_maximum_search_steps,
                            input.context.optimizer_maximum_candidate_count));
  const std::size_t maximum_decoded_bytes =
      bounded_size(input.context.optimizer_memory_budget_bytes);
  const std::size_t maximum_output_rows =
      bounded_size(input.context.optimizer_maximum_candidate_count);
  const std::size_t maximum_output_columns = dag.outputs.size();
  if (maximum_scanned_row_versions == 0 || maximum_decoded_bytes == 0 ||
      maximum_output_rows == 0 || maximum_output_columns == 0 ||
      maximum_output_rows >
          std::numeric_limits<std::size_t>::max() / maximum_output_columns) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "object-backed heap execution bounds are absent or overflow");
  }

  api::CanonicalHeapOptimizerSelectedExecutionRequest execution_request;
  execution_request.context = input.context;
  execution_request.relational_dag = input.relational_dag;
  execution_request.selected_physical_dag = physical.physical_dag;
  execution_request.maximum_scanned_row_versions =
      maximum_scanned_row_versions;
  execution_request.maximum_decoded_bytes = maximum_decoded_bytes;
  execution_request.maximum_output_rows = maximum_output_rows;
  execution_request.maximum_output_columns = maximum_output_columns;
  execution_request.maximum_output_cells =
      maximum_output_rows * maximum_output_columns;
  execution_request.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  execution_request.execution_attempt_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + input.context.current_monotonic_ns,
      "heap-scan.execution-attempt");
  execution_request.transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(input.context.local_transaction_id) + ":" +
          std::to_string(
              input.context.snapshot_visible_through_local_transaction_id),
      "heap-scan.transaction-effect-unchanged");

  const auto execution =
      api::ExecuteCanonicalHeapOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-PACKET7-OBJECT-HEAP-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "object-backed heap selected DAG was not completed"
            : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published = execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(planning_request, execution);
  return result;
#endif
}

}  // namespace scratchbird::engine::sblr
