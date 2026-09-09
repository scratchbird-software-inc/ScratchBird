// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_runtime_observation_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RUNTIME_OBSERVATION_SUPPORT_AUTHORITY
// Wraps admitted callbacks with exact producer-memory observations.
// Borrows receipts through synchronous dispatch; owns no optimizer grant,
// plan selection, storage access, snapshot construction, or transaction finality.

void PublishOrdinaryRuntimeObservations(
    std::vector<exec::CanonicalPhysicalExecutorRegistration>* registrations,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts) {
  if (registrations == nullptr) return;
  namespace metric = scratchbird::engine::executor;
  const auto observed = [](const std::uint64_t value) {
    return metric::CanonicalObservedUint64{
        metric::CanonicalRuntimeMetricState::kObserved, value};
  };
  const auto not_applicable = [] {
    return metric::CanonicalObservedUint64{
        metric::CanonicalRuntimeMetricState::kNotApplicable, 0};
  };
  const auto* borrowed_memory_receipts = &memory_receipts;
  for (auto& registration : *registrations) {
    if (!registration.engine_owned || !registration.execute ||
        registration.implementation_id.starts_with("scan.heap") ||
        registration.publishes_runtime_observation_v1 ||
        registration.honors_dispatcher_memory_limit_v1) {
      continue;
    }
    // Memory-limited live callbacks perform complete conservative admission
    // internally, but do not carry an allocator-observed exact peak.
    // Do not relabel a reconstructed batch/pair estimate as an exact producer
    // receipt; runtime feedback remains unavailable for this node instead.
    const bool has_producer_receipt = std::ranges::any_of(
        memory_receipts, [&](const auto& item) {
          return item.second.implementation_id ==
                     registration.implementation_id &&
                 item.second.producer_peak_memory_exact;
        });
    if (!has_producer_receipt) continue;
    auto execute = std::move(registration.execute);
    registration.execute =
        [execute = std::move(execute), observed, not_applicable,
         borrowed_memory_receipts](
            const exec::TypedPhysicalNodeDag& dag,
            const exec::PhysicalNodeRecord& node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
          auto step = execute(dag, node, inputs);
          if (!step.diagnostic.ok) return step;
          const auto memory =
              borrowed_memory_receipts->find(node.physical_node_id);
          if (memory == borrowed_memory_receipts->end() ||
              memory->second.implementation_id != node.implementation_id ||
              memory->second.physical_node_id != node.physical_node_id ||
              !memory->second.producer_peak_memory_exact ||
              !step.materialized_output_batch.has_value()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "runtime_observation_producer_memory_receipt";
            return step;
          }
          std::uint64_t current_memory_bytes = 0;
          if (!RuntimeMaterializedBatchMemoryBytes(
                  *step.materialized_output_batch,
                  &current_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "runtime_observation_current_memory_receipt";
            return step;
          }
          // The exact callback peak is calculated before publication and
          // carried independently beside the optimizer grant. Never
          // reconstruct it from the DAG grant or blanket-charge immutable
          // shared dispatch inputs.
          std::uint64_t accounted_auxiliary_memory_bytes =
              memory->second.accounted_auxiliary_memory_bytes;
          std::uint64_t peak_memory_bytes =
              memory->second.producer_peak_memory_bytes;
          if (memory->second.producer_peak_from_runtime_batches) {
            peak_memory_bytes = current_memory_bytes;
            for (const auto& input : inputs) {
              std::uint64_t input_bytes = 0;
              if (!input.materialized_output_batch.has_value() ||
                  !RuntimeMaterializedBatchMemoryBytes(
                      *input.materialized_output_batch, &input_bytes) ||
                  input_bytes > std::numeric_limits<std::uint64_t>::max() -
                                    peak_memory_bytes) {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "QOW-DIAG-OPT-017-REFUSAL-V1";
                step.diagnostic.detail =
                    "runtime_observation_dynamic_input_peak";
                return step;
              }
              peak_memory_bytes += input_bytes;
            }
            std::uint64_t runtime_work_bytes = 0;
            if (node.node_kind == exec::PhysicalNodeKind::kFilter &&
                !inputs.empty()) {
              if (!CheckedMultiply(
                      inputs.front().materialized_output_batch->rows.size(),
                      sizeof(api::EngineSqlTruthValue),
                      &runtime_work_bytes)) {
                step.diagnostic.ok = false;
              }
            } else if (node.node_kind == exec::PhysicalNodeKind::kSort &&
                       !inputs.empty()) {
              const auto rows =
                  inputs.front().materialized_output_batch->rows.size();
              std::uint64_t comparisons = 0;
              std::uint64_t order = 0;
              if (!CheckedMultiply(rows, rows, &comparisons) ||
                  !CheckedMultiply(rows, sizeof(std::size_t), &order) ||
                  !CheckedAdd(comparisons, order, &runtime_work_bytes)) {
                step.diagnostic.ok = false;
              }
            } else if (node.node_kind == exec::PhysicalNodeKind::kJoin &&
                       inputs.size() == 2) {
              std::uint64_t pairs = 0;
              const bool correlated_equality =
                  node.implementation_id ==
                      "join.lateral-inner.correlated.typed.v1" ||
                  node.implementation_id ==
                      "join.lateral-left.correlated.typed.v1" ||
                  node.implementation_id ==
                      "join.cross-apply.correlated.typed.v1" ||
                  node.implementation_id ==
                      "join.outer-apply.correlated.typed.v1";
              static constexpr std::array<std::string_view, 7>
                  kOrdinaryJoinImplementationIds = {{
                      "join.cross.3vl.nested.v1",
                      "join.inner.3vl.nested.v1",
                      "join.left-outer.3vl.nested.v1",
                      "join.right-outer.3vl.nested.v1",
                      "join.full-outer.3vl.nested.v1",
                      "join.left-semi.3vl.nested.v1",
                      "join.left-anti.3vl.nested.v1",
                  }};
              const bool ordinary_join = std::ranges::any_of(
                  kOrdinaryJoinImplementationIds,
                  [&](const auto implementation_id) {
                    return node.implementation_id == implementation_id;
                  });
              const bool descriptor_bound_correlation =
                  correlated_equality &&
                  !inputs[0].materialized_output_batch->columns.empty() &&
                  !inputs[1].materialized_output_batch->columns.empty() &&
                  CanonicalRelationalComparisonAuthorityRequiredV1(
                      inputs[0]
                          .materialized_output_batch->columns.front()
                          .descriptor,
                      inputs[1]
                          .materialized_output_batch->columns.front()
                          .descriptor);
              const bool pair_count_valid = CheckedMultiply(
                  inputs[0].materialized_output_batch->rows.size(),
                  inputs[1].materialized_output_batch->rows.size(), &pairs);
              if (!pair_count_valid) {
                step.diagnostic.ok = false;
              } else if (correlated_equality) {
                // Descriptor-bound correlation retains one comparison result
                // per candidate pair. Scalar equality is evaluated directly
                // and retains no pair buffer.
                if (descriptor_bound_correlation &&
                    !CheckedMultiply(pairs, sizeof(std::optional<int>),
                                     &runtime_work_bytes)) {
                  step.diagnostic.ok = false;
                }
              } else if (ordinary_join &&
                         !exec::BoundCanonicalJoinRetainedStateBytes(
                             inputs[0]
                                 .materialized_output_batch->rows.size(),
                             inputs[1]
                                 .materialized_output_batch->rows.size(),
                             &runtime_work_bytes)) {
                step.diagnostic.ok = false;
              } else if (!ordinary_join &&
                         !CheckedMultiply(
                             pairs, sizeof(api::EngineSqlTruthValue),
                             &runtime_work_bytes)) {
                // Preserve the existing conservative pair-truth fallback for
                // custom JOIN implementations (for example ASOF).
                step.diagnostic.ok = false;
              }
              if (step.diagnostic.ok &&
                  (correlated_equality || ordinary_join)) {
                accounted_auxiliary_memory_bytes = runtime_work_bytes;
              }
            } else if (
                node.node_kind == exec::PhysicalNodeKind::kAggregate &&
                node.implementation_id ==
                    "aggregate.query-distinct.typed.v1") {
              if (inputs.size() != 1 ||
                  !inputs.front().materialized_output_batch.has_value() ||
                  !QueryDistinctAuxiliaryMemoryBytes(
                      inputs.front().materialized_output_batch->rows.size(),
                      inputs.front().materialized_output_batch->columns.size(),
                      &runtime_work_bytes)) {
                step.diagnostic.ok = false;
              } else {
                accounted_auxiliary_memory_bytes = runtime_work_bytes;
              }
            } else if (node.node_kind ==
                       exec::PhysicalNodeKind::kAggregate) {
              // Heap aggregate profiles carry one scalar integer accumulator.
              // Non-exact composed aggregates additionally carry their exact
              // admitted auxiliary work receipt beside the runtime batches.
              runtime_work_bytes = std::max<std::uint64_t>(
                  sizeof(std::int64_t),
                  memory->second.accounted_auxiliary_memory_bytes);
            } else if (node.node_kind ==
                       exec::PhysicalNodeKind::kSubquery) {
              // Predicate and cardinality subqueries retain one canonical
              // table-result copy (and, when applicable, a descriptor-bound
              // comparison carrier) while constructing their callback output.
              runtime_work_bytes =
                  memory->second.accounted_auxiliary_memory_bytes;
              if (memory->second
                      .accounted_auxiliary_from_first_input_batch) {
                std::uint64_t intermediate_table_bytes = 0;
                if (inputs.size() != 1 ||
                    !inputs.front().materialized_output_batch.has_value() ||
                    !RuntimeMaterializedBatchMemoryBytes(
                        *inputs.front().materialized_output_batch,
                        &intermediate_table_bytes) ||
                    !CheckedAdd(runtime_work_bytes,
                                intermediate_table_bytes,
                                &runtime_work_bytes)) {
                  step.diagnostic.ok = false;
                }
              }
              if (step.diagnostic.ok) {
                accounted_auxiliary_memory_bytes = runtime_work_bytes;
              }
            } else if (
                node.node_kind == exec::PhysicalNodeKind::kCte &&
                memory->second.accounted_auxiliary_from_first_input_batch &&
                !inputs.empty()) {
              // A shareable nonrecursive CTE owns a materialized copy in
              // addition to the immutable dispatch input and callback output.
              // Inline CTEs carry no such auxiliary receipt.
              if (!RuntimeMaterializedBatchMemoryBytes(
                      *inputs.front().materialized_output_batch,
                      &runtime_work_bytes)) {
                step.diagnostic.ok = false;
              } else {
                accounted_auxiliary_memory_bytes = runtime_work_bytes;
              }
            }
            if (!step.diagnostic.ok ||
                runtime_work_bytes >
                    std::numeric_limits<std::uint64_t>::max() -
                        peak_memory_bytes) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-OPT-017-REFUSAL-V1";
              step.diagnostic.detail =
                  "runtime_observation_dynamic_work_peak";
              return step;
            }
            peak_memory_bytes += runtime_work_bytes;
          }
          if (current_memory_bytes > peak_memory_bytes ||
              accounted_auxiliary_memory_bytes >
                  peak_memory_bytes - current_memory_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "runtime_observation_current_exceeds_producer_peak";
            return step;
          }
          auto& receipt = step.runtime_observation;
          receipt.abi_version = 1;
          receipt.operator_wait_ns = observed(0);
          receipt.current_memory_bytes = observed(current_memory_bytes);
          receipt.peak_memory_bytes = observed(peak_memory_bytes);
          receipt.decoded_bytes = not_applicable();
          receipt.bytes_read = not_applicable();
          receipt.bytes_written = not_applicable();
          receipt.pages_read = step.pages_read == 0
                                   ? not_applicable()
                                   : observed(step.pages_read);
          receipt.pages_written = not_applicable();
          receipt.spill_bytes_read = observed(0);
          receipt.spill_bytes_written = observed(step.spill_bytes);
          receipt.visibility_recheck_count = not_applicable();
          receipt.security_recheck_count = not_applicable();
          receipt.storage_recheck_count = not_applicable();
          receipt.index_recheck_count = not_applicable();
          receipt.residual_recheck_count =
              memory->second.residual_recheck_applicable
                  ? observed(step.rows_examined)
                  : not_applicable();
          receipt.compatibility_recheck_count = not_applicable();
          receipt.archive_bytes_read = not_applicable();
          receipt.cluster_bytes_sent = not_applicable();
          receipt.cluster_bytes_received = not_applicable();
          receipt.authority.engine_execution_observation = true;
          receipt.producer_receipt_complete = true;
          if (!step.data_access_observation_known &&
              memory->second.non_accessing_proven) {
            step.data_access_observation_known = true;
            step.data_access_observed = false;
          }
          return step;
        };
    if (registration.retained_live_memory_bytes_v1 != 0) {
      constexpr std::uint64_t kObservationWrapperRetainedBytes =
          sizeof(exec::CanonicalPhysicalNodeExecutor) + 8 * sizeof(void*) +
          256;
      if (!CheckedAdd(registration.retained_live_memory_bytes_v1,
                      kObservationWrapperRetainedBytes,
                      &registration.retained_live_memory_bytes_v1)) {
        registration.retained_live_memory_bytes_v1 = 0;
      }
    }
    registration.publishes_runtime_observation_v1 = true;
  }
}

bool HasOrdinaryRuntimeObservationWrapperTarget(
    const std::vector<exec::CanonicalPhysicalExecutorRegistration>&
        registrations,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts) {
  return std::ranges::any_of(registrations, [&](const auto& registration) {
    if (!registration.engine_owned || !registration.execute ||
        registration.implementation_id.starts_with("scan.heap") ||
        registration.publishes_runtime_observation_v1 ||
        registration.honors_dispatcher_memory_limit_v1) {
      return false;
    }
    return std::ranges::any_of(memory_receipts, [&](const auto& item) {
      return item.second.implementation_id == registration.implementation_id &&
             item.second.producer_peak_memory_exact;
    });
  });
}

}  // namespace scratchbird::engine::sblr
