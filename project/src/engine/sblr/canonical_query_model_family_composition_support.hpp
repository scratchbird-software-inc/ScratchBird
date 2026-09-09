// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"

#include "engine/executor/model_family_executor.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"
#include "engine/optimizer/model_family_profile_factory.hpp"
#include "engine/optimizer/relational_planner.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// Immutable cross-family capture carriers. They contain an already-selected
// model leg and execution request but own no planning, snapshot construction,
// storage publication, or transaction finality.
struct Rcp079ColumnarSourceRuntimeMemoryReceiptV1 {
  bool complete{false};
  std::uint64_t provider_logical_memory_bytes{0};
  std::uint64_t peak_live_memory_bytes{0};
  std::uint64_t memory_grant_bytes{0};
};

struct Rcp079CapturedModelLegV1 {
  bool captured{false};
  std::uint32_t logical_node_id{0};
  std::string family_id;
  std::string implementation_id;
  std::string capability_uuid;
  std::string transformation_rule_id;
  std::string compatibility_profile_id;
  std::string current_relation_descriptor_uuid;
  std::uint64_t current_relation_descriptor_generation{0};
  plan::CanonicalLogicalRelationalNodeKind logical_node_kind{
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource};
  exec::PhysicalNodeKind physical_node_kind{exec::PhysicalNodeKind::kScan};
  exec::ModelFamilyExecutionRequestV1 execution_request;
  std::vector<exec::ExecutorColumnDescriptor> exact_output_columns;
  std::shared_ptr<Rcp079ColumnarSourceRuntimeMemoryReceiptV1>
      columnar_runtime_memory_receipt;
  std::shared_ptr<std::atomic_bool> cancellation_probe_failed;
};

enum class LiveCancellationProbeState : std::uint8_t {
  kRunning = 0,
  kCancelled,
  kProbeFailed,
};

// Narrow planning adapters consume engine-owned catalog/authorization facts
// and return optimizer-owned capability/selection receipts. They cannot grant
// access, create an MGA snapshot, or execute a physical node.
opt::ModelFamilyCapabilitySnapshotV1
MakeModelFamilyCapabilitySnapshotForCompositionV1(
    const opt::ModelFamilyCoordinatorRequestV1& planning,
    std::string_view identity_scope,
    opt::ModelFamilyAlternativeRouteClassV1 route_class,
    std::string provider_uuid,
    std::string capability_uuid,
    std::uint64_t provider_generation,
    bool available,
    std::uint64_t work_units,
    std::uint64_t sequential_pages,
    std::uint64_t memory_bytes_required);

opt::ModelFamilyCoordinatorResultV1
PlanCanonicalModelFamilySourceForCompositionV1(
    opt::ModelFamilyCoordinatorRequestV1 planning,
    std::string identity_scope,
    std::vector<opt::ModelFamilyCapabilitySnapshotV1> snapshots);

void CaptureRcp079ModelLegV1(
    Rcp079CapturedModelLegV1* capture,
    std::uint32_t logical_node_id,
    std::string family_id,
    std::string implementation_id,
    std::string transformation_rule_id,
    std::string compatibility_profile_id,
    std::string relation_descriptor_uuid,
    std::uint64_t relation_descriptor_generation,
    plan::CanonicalLogicalRelationalNodeKind logical_node_kind,
    exec::PhysicalNodeKind physical_node_kind,
    const exec::ModelFamilyExecutionRequestV1& execution_request);

bool Rcp079ExactExecutorColumnV1(
    const exec::ExecutorColumnDescriptor& actual,
    const exec::ExecutorColumnDescriptor& expected);

std::optional<std::uint64_t> Rcp079ModelProviderBatchLogicalMemoryBytesV1(
    const exec::ModelProviderBatchV1& batch);

std::optional<std::uint64_t>
Rcp079ProjectedModelSourceOutputLogicalMemoryBytesV1(
    const exec::ModelSourceInputDescriptorV1& input,
    const exec::ModelProviderBatchV1& provider);

bool PollLiveCancellationProbe(
    const std::function<bool()>& cancellation_requested,
    LiveCancellationProbeState* state) noexcept;

}  // namespace scratchbird::engine::sblr
