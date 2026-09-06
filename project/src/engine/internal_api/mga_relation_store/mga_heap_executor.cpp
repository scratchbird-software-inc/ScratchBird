// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"

#include "api_diagnostics.hpp"
#include "agents/index_garbage_cleanup_agent.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "descriptor_value_runtime.hpp"
#include "ipar_fault_injection.hpp"
#include "local_transaction_store.hpp"
#include "query/contextual_text_policy_registry_v2.hpp"
#include "query/contextual_text_target_authority_resolver_v2.hpp"
#include "query/plan_api.hpp"
#include "secondary_index_delta_merge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"
#include "whole_store_crash_injection.hpp"

// SEARCH_KEY: SB_ENGINE_MGA_HEAP_EXECUTION_AUTHORITY
// Owns executor-facing heap acquisition, physical-node dispatch, and result
// projection. Durable row-version and transaction-finality authority remain in
// the internal MGA relation-store facade and transaction inventory.

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic HeapAcquisitionRefusal(std::string code,
                                                   std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

PhysicalMgaStatementContext PhysicalMgaContextFromResolvedSnapshot(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor) {
  PhysicalMgaStatementContext expected;
  expected.statement_uuid = context.statement_uuid.canonical;
  expected.owning_transaction_uuid = context.transaction_uuid.canonical;
  expected.statement_snapshot_uuid =
      context.statement_snapshot_uuid.canonical;
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

DescriptorRuntimeDiagnostic ValidateCurrentHeapPhysicalMgaAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const TypedPhysicalNodeDag& physical_dag) {
  namespace api = scratchbird::engine::internal_api;
  api::EngineResolveStatementSnapshotRequest resolve_request;
  resolve_request.context = context;
  const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
  if (!resolved.ok) {
    return HeapAcquisitionRefusal(
        "SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
        "statement snapshot is unknown, revoked, stale, or not current");
  }
  const auto expected = PhysicalMgaContextFromResolvedSnapshot(
      context, resolved.snapshot_vector);
  if (!PhysicalMgaStatementContextEqual(physical_dag.mga_statement_context,
                                        expected)) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
        "physical MGA statement context differs from current inventory authority");
  }
  return {};
}

struct CurrentHeapMgaResolutionBinding {
  scratchbird::engine::internal_api::EngineTrustMode trust_mode =
      scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  std::string database_path;
  std::string transaction_uuid;
  std::string statement_uuid;
  std::string statement_snapshot_uuid;
  std::string statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t visible_committed_high_watermark{0};
};

CanonicalExecutionMgaAuthority BuildCurrentHeapExecutionMgaAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const TypedPhysicalNodeDag& physical_dag) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalExecutionMgaAuthority authority;
  authority.statement_context = physical_dag.mga_statement_context;
  authority.origin = CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  CurrentHeapMgaResolutionBinding binding;
  binding.trust_mode = context.trust_mode;
  binding.database_path = context.database_path;
  binding.transaction_uuid = context.transaction_uuid.canonical;
  binding.statement_uuid = context.statement_uuid.canonical;
  binding.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  binding.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  binding.local_transaction_id = context.local_transaction_id;
  binding.visible_committed_high_watermark =
      context.snapshot_visible_through_local_transaction_id;
  authority.resolve_current = [binding = std::move(binding)]() {
    CanonicalMgaCurrentResolution current;
    api::EngineRequestContext context;
    context.trust_mode = binding.trust_mode;
    context.database_path = binding.database_path;
    context.transaction_uuid.canonical = binding.transaction_uuid;
    context.statement_uuid.canonical = binding.statement_uuid;
    context.statement_snapshot_uuid.canonical =
        binding.statement_snapshot_uuid;
    context.statement_metadata_snapshot_uuid.canonical =
        binding.statement_metadata_snapshot_uuid;
    context.local_transaction_id = binding.local_transaction_id;
    context.snapshot_visible_through_local_transaction_id =
        binding.visible_committed_high_watermark;
    api::EngineResolveStatementSnapshotRequest resolve_request;
    resolve_request.context = context;
    const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
    if (!resolved.ok) {
      current.diagnostic = HeapAcquisitionRefusal(
          "SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
          "statement snapshot is unknown, revoked, stale, or not current");
      return current;
    }
    current.statement_context = PhysicalMgaContextFromResolvedSnapshot(
        context, resolved.snapshot_vector);
    return current;
  };
  return authority;
}

bool IsCanonicalHeapBindingUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) { continue; }
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) { return false; }
  }
  return true;
}

std::optional<std::string_view> ExactHeapDescriptorField(
    const std::string_view descriptor,
    const std::string_view field_name) {
  std::optional<std::string_view> value;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.size() > field_name.size() &&
        field.starts_with(field_name) && field[field_name.size()] == '=') {
      if (value.has_value() || field.size() == field_name.size() + 1) {
        return std::nullopt;
      }
      value = field.substr(field_name.size() + 1);
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  return value;
}

bool CheckedHeapBoundToU64(const std::size_t value, std::uint64_t* output) {
  if (output == nullptr ||
      value > static_cast<std::size_t>(
                  std::numeric_limits<std::uint64_t>::max())) {
    return false;
  }
  *output = static_cast<std::uint64_t>(value);
  return true;
}

constexpr std::size_t kMaximumHeapOutputColumns = 4096;

bool CheckedHeapCellCount(const std::size_t row_count,
                          const std::size_t column_count,
                          std::size_t* output) {
  if (output == nullptr ||
      (column_count != 0 &&
       row_count > std::numeric_limits<std::size_t>::max() / column_count)) {
    return false;
  }
  *output = row_count * column_count;
  return true;
}

bool ExactOptionalHeapDescriptorFieldMatches(
    const std::string_view descriptor,
    const std::string_view field_name,
    const std::optional<std::string>& expected) {
  std::size_t matches = 0;
  std::string_view actual;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    if (field.size() > field_name.size() &&
        field.starts_with(field_name) && field[field_name.size()] == '=') {
      ++matches;
      actual = field.substr(field_name.size() + 1);
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  if (!expected.has_value()) { return matches == 0; }
  return matches == 1 && !actual.empty() && actual == *expected;
}

bool ExactHeapNullabilityCarrierMatches(const std::string_view descriptor,
                                        const bool expected_nullable) {
  std::optional<bool> admitted;
  std::size_t canonical_count = 0;
  std::size_t storage_count = 0;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto next = descriptor.find(';', offset);
    const auto end = next == std::string_view::npos ? descriptor.size() : next;
    const auto field = descriptor.substr(offset, end - offset);
    std::optional<bool> current;
    if (field.rfind("nullability=", 0) == 0) {
      ++canonical_count;
      const auto value = field.substr(std::string_view("nullability=").size());
      if (value == "nullable") {
        current = true;
      } else if (value == "non_null") {
        current = false;
      } else {
        return false;
      }
    } else if (field.rfind("nullable=", 0) == 0) {
      ++storage_count;
      const auto value = field.substr(std::string_view("nullable=").size());
      if (value == "true") {
        current = true;
      } else if (value == "false") {
        current = false;
      } else {
        return false;
      }
    }
    if (current.has_value()) {
      if (admitted.has_value() && *admitted != *current) { return false; }
      admitted = current;
    }
    if (next == std::string_view::npos) { break; }
    offset = next + 1;
  }
  return canonical_count <= 1 && storage_count <= 1 &&
         admitted.has_value() && *admitted == expected_nullable;
}

}  // namespace

// QOW-SOURCE-QRY-004-HEAP-MGA-V1
static CanonicalHeapRelationAcquisitionResult
ExecuteCanonicalHeapRelationAcquisitionPrepared(
    const CanonicalHeapRelationAcquisitionRequest& request,
    const scratchbird::engine::internal_api::PreparedMgaHeapReadAuthority*
        prepared_read_authority) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalHeapRelationAcquisitionResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool data_access_observed = false,
                          const bool cancellation_observed = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.data_access_observed = data_access_observed;
    result.cancellation_observed = cancellation_observed;
    return result;
  };
  const auto invalid = [&](std::string code,
                           std::string detail,
                           const bool data_access_observed = false,
                           const bool cancellation_observed = false) {
    return refuse(HeapAcquisitionRefusal(std::move(code), std::move(detail)),
                  data_access_observed,
                  cancellation_observed);
  };

  if (request.context == nullptr || request.relational_dag == nullptr) {
    return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                   "engine context and typed relational DAG are required");
  }
  const auto& context = *request.context;
  const auto& physical_dag = request.borrowed_physical_dag == nullptr
                                 ? request.physical_dag
                                 : *request.borrowed_physical_dag;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto& mga_authority =
      request.borrowed_mga_authority == nullptr
          ? request.mga_authority
          : *request.borrowed_mga_authority;
  auto maximum_live_memory_bytes =
      request.maximum_live_memory_bytes == 0
          ? context.optimizer_memory_budget_bytes
          : request.maximum_live_memory_bytes;
  const auto& relational = *request.relational_dag;
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  if (mga_authority.origin !=
      CanonicalMgaAuthorityOrigin::kEngineTransactionInventory) {
    return invalid("QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
                   "heap acquisition requires engine transaction-inventory authority");
  }
  if (prepared_read_authority == nullptr) {
    const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!entry_authority.ok) return refuse(entry_authority);
    const auto relational_validation =
        api::ValidateTypedRelationalDag(relational);
    if (!relational_validation.accepted) {
      const auto& issue = relational_validation.issues.front();
      return invalid(issue.diagnostic_id,
                     issue.field_id + ":node_id=" +
                         std::to_string(issue.node_id));
    }
    const auto physical_validation = ValidateTypedPhysicalNodeDag(physical_dag);
    if (!physical_validation.accepted) {
      const auto& issue = physical_validation.issues.front();
      return invalid(issue.diagnostic_id, issue.field_id);
    }
    const auto current_mga =
        ValidateCurrentHeapPhysicalMgaAuthority(context, physical_dag);
    if (!current_mga.ok) return refuse(current_mga);
  }
  if (relational.wire_version != 2 ||
      physical_dag.abi_version != 2 ||
      !physical_dag.optimizer_published ||
      !physical_dag.immutable_node_identity_validated ||
      !physical_dag.capability_validated_before_access ||
      physical_dag.data_access_observed) {
    return invalid("QOW-DIAG-QRY-004-HEAP-DIRECT-SCOPE-V1",
                   "wire-v2 direct optimizer publication is required");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return invalid("QOW-DIAG-QRY-004-HEAP-DIRECT-SCOPE-V1",
                   "prepared or cached descriptor execution is outside this profile");
  }
  if (!cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > kMaximumHeapOutputColumns) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "valid nonzero heap row, column, cell, byte, scan, and "
                   "cancellation bounds are required");
  }
  if (cancellation_requested()) {
    return invalid("QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
                   "heap relation acquisition cancelled before admission",
                   false,
                   true);
  }
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      physical_dag.local_transaction_id !=
          context.local_transaction_id ||
      physical_dag.statement_snapshot_id !=
          context.snapshot_visible_through_local_transaction_id) {
    return invalid("SB_DIAG_MGA_READ_SNAPSHOT_MISSING",
                   "physical scan does not match the active MGA transaction snapshot");
  }
  const auto& authorization = context.authorization_context;
  if (!context.statement_metadata_snapshot_engine_owned ||
      !context.security_context_present || !authorization.present ||
      relational.bound_sblr_tree_uuid !=
          physical_dag.bound_sblr_tree_uuid ||
      relational.statement_uuid != context.statement_uuid.canonical ||
      relational.owning_transaction_uuid !=
          context.transaction_uuid.canonical ||
      relational.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      relational.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      relational.local_transaction_id != context.local_transaction_id ||
      relational.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id ||
      relational.bound_catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      physical_dag.catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      relational.bound_security_context_uuid !=
          authorization.authority_uuid.canonical ||
      physical_dag.security_context_uuid !=
          authorization.authority_uuid.canonical ||
      physical_dag.catalog_generation !=
          context.catalog_generation_id ||
      context.catalog_generation_id !=
          authorization.catalog_generation_id ||
      physical_dag.security_epoch != context.security_epoch ||
      context.security_epoch != authorization.security_epoch ||
      physical_dag.policy_epoch != authorization.policy_epoch ||
      physical_dag.resource_epoch != context.resource_epoch ||
      physical_dag.capability_snapshot_uuid !=
          context.optimizer_capability_snapshot_uuid.canonical ||
      physical_dag.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      physical_dag.route_snapshot_uuid !=
          context.optimizer_route_snapshot_uuid.canonical ||
      physical_dag.route_epoch != context.optimizer_route_epoch ||
      physical_dag.route_generation !=
          context.optimizer_route_generation ||
      physical_dag.memory_budget_bytes !=
          context.optimizer_memory_budget_bytes) {
    return invalid("QOW-DIAG-QRY-004-HEAP-AUTHORITY-SCOPE-V1",
                   "catalog, security, resource, or MGA scope is stale or mismatched");
  }
  if (context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      request.maximum_decoded_bytes > context.optimizer_memory_budget_bytes ||
      request.maximum_decoded_bytes > maximum_live_memory_bytes ||
      maximum_live_memory_bytes > context.optimizer_memory_budget_bytes ||
      request.maximum_scanned_row_versions >
          context.optimizer_maximum_candidate_count ||
      request.maximum_output_rows >
          context.optimizer_maximum_candidate_count) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap scan bounds exceed the admitted optimizer resources");
  }

  const auto physical = std::ranges::find_if(
      physical_dag.nodes, [&](const auto& node) {
        return node.physical_node_id == request.selected_physical_node_id;
      });
  if (request.selected_physical_node_id == 0 ||
      physical == physical_dag.nodes.end() ||
      physical->node_kind != PhysicalNodeKind::kScan ||
      (physical->implementation_id != "scan.heap.v1" &&
       physical->implementation_id !=
           "scan.heap.tablesample.bernoulli.v1" &&
       physical->implementation_id !=
           "scan.heap.tablesample.system.v1") ||
      !physical->input_physical_node_ids.empty() ||
      !physical->engine_capability_validated) {
    return invalid("QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
                   "selected physical node is not an admitted leaf heap scan");
  }
  if (physical->memory_bytes_required == 0) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "selected heap node has no live-memory grant");
  }
  // The selected physical node is always the operator-local ceiling. A zero
  // request inherits the statement budget, but never bypasses the node grant.
  maximum_live_memory_bytes =
      std::min(maximum_live_memory_bytes, physical->memory_bytes_required);
  const auto maximum_decoded_bytes =
      std::min(request.maximum_decoded_bytes, maximum_live_memory_bytes);
  if (maximum_decoded_bytes == 0) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "selected heap node has no decoded-memory allowance");
  }
  const auto relation_node = std::ranges::find_if(
      relational.nodes, [&](const auto& node) {
        return node.node_id == physical->relational_node_id;
      });
  const bool base_heap_source =
      relation_node != relational.nodes.end() &&
      relation_node->semantic_variant_id == "relation.source.v1" &&
      physical->implementation_id == "scan.heap.v1";
  const bool bernoulli_heap_source =
      relation_node != relational.nodes.end() &&
      relation_node->semantic_variant_id ==
          "relation.source.tablesample.bernoulli.v1" &&
      physical->implementation_id ==
          "scan.heap.tablesample.bernoulli.v1";
  const bool system_heap_source =
      relation_node != relational.nodes.end() &&
      relation_node->semantic_variant_id ==
          "relation.source.tablesample.system.v1" &&
      physical->implementation_id ==
          "scan.heap.tablesample.system.v1";
  if (relation_node == relational.nodes.end() ||
      relation_node->node_kind != api::RelationalDagNodeKind::kScan ||
      !relation_node->input_node_ids.empty() ||
      (!base_heap_source && !bernoulli_heap_source && !system_heap_source) ||
      relation_node->required_object_uuids.size() != 1 ||
      relation_node->output_descriptor_ids.empty() ||
      relation_node->output_descriptor_ids.size() !=
          relation_node->bound_expression_ids.size() ||
      relation_node->output_descriptor_ids.size() >
          request.maximum_output_columns ||
      relation_node->output_descriptor_ids.size() >
          kMaximumHeapOutputColumns ||
      physical->output_descriptor_ids != relation_node->output_descriptor_ids) {
    return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                   "selected relation source binding is not exact");
  }
  const std::string& relation_uuid =
      relation_node->required_object_uuids.front();
  if (!IsCanonicalHeapBindingUuid(relation_uuid)) {
    return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                   "bound relation UUID is not canonical");
  }

  const auto output_width = relation_node->output_descriptor_ids.size();
  std::size_t admitted_shape_cells = 0;
  if (!CheckedHeapCellCount(request.maximum_output_rows, output_width,
                            &admitted_shape_cells)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "admitted heap row-by-width shape overflows size_t");
  }

  // QOW-SOURCE-QRY-004-HEAP-PROJECTED-WIDTH-V1
  // The leaf scan accepts one visible, one-to-one mapping in requested output
  // order from optimizer outputs through identifier bindings and descriptors.
  // Persisted column UUIDs select a bounded subset; aliases, hidden outputs,
  // and duplicate bindings remain outside this profile.
  for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
    const auto output = std::ranges::find_if(
        relational.outputs, [&](const auto& candidate) {
          return candidate.relation_node_id == relation_node->node_id &&
                 candidate.ordinal == ordinal;
        });
    const auto expression = std::ranges::find_if(
        relational.expressions, [&](const auto& candidate) {
          return candidate.expression_id ==
                 relation_node->bound_expression_ids[ordinal];
        });
    const auto descriptor = std::ranges::find_if(
        relational.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id ==
                 relation_node->output_descriptor_ids[ordinal];
        });
    if (output == relational.outputs.end() ||
        expression == relational.expressions.end() ||
        descriptor == relational.descriptors.end()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                     "relation source output binding is incomplete");
    }
    bool duplicate_identity = false;
    for (std::size_t prior = 0; prior < ordinal; ++prior) {
      const auto prior_output = std::ranges::find_if(
          relational.outputs, [&](const auto& candidate) {
            return candidate.relation_node_id == relation_node->node_id &&
                   candidate.ordinal == prior;
          });
      const auto prior_expression = std::ranges::find_if(
          relational.expressions, [&](const auto& candidate) {
            return candidate.expression_id ==
                   relation_node->bound_expression_ids[prior];
          });
      const auto prior_descriptor = std::ranges::find_if(
          relational.descriptors, [&](const auto& candidate) {
            return candidate.descriptor_id ==
                   relation_node->output_descriptor_ids[prior];
          });
      if (prior_output == relational.outputs.end() ||
          prior_expression == relational.expressions.end() ||
          prior_descriptor == relational.descriptors.end() ||
          prior_output->output_id == output->output_id ||
          prior_expression->expression_id == expression->expression_id ||
          prior_descriptor->descriptor_id == descriptor->descriptor_id ||
          (prior_expression->bound_name_uuid.has_value() &&
           expression->bound_name_uuid.has_value() &&
           *prior_expression->bound_name_uuid ==
               *expression->bound_name_uuid)) {
        duplicate_identity = true;
        break;
      }
    }
    if (!output->visible || output->output_id == 0 ||
        duplicate_identity ||
        output->output_name_utf8.empty() ||
        output->descriptor_id != relation_node->output_descriptor_ids[ordinal] ||
        expression->expression_id != output->expression_id ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        !expression->bound_name_uuid.has_value() ||
        !IsCanonicalHeapBindingUuid(*expression->bound_name_uuid) ||
        expression->result_descriptor_id != output->descriptor_id ||
        relation_node->bound_expression_ids[ordinal] !=
            expression->expression_id ||
        descriptor->descriptor_id != output->descriptor_id ||
        !IsCanonicalHeapBindingUuid(descriptor->descriptor_uuid) ||
        !IsCanonicalHeapBindingUuid(descriptor->type_uuid) ||
        descriptor->nullability == api::RelationalNullability::kUnknown ||
        (descriptor->collation_uuid.has_value() &&
         !IsCanonicalHeapBindingUuid(*descriptor->collation_uuid)) ||
        (descriptor->timezone_profile_id.has_value() &&
         descriptor->timezone_profile_id->empty())) {
      return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                     "relation output binding is incomplete or not bijective");
    }
  }

  if (prepared_read_authority == nullptr) {
    const auto authorization_decision = api::EvaluateMaterializedAuthorization(
        context, authorization, "SELECT", relation_uuid);
    if (!authorization_decision.authorized || authorization_decision.denied ||
        authorization_decision.policy_recheck_required ||
        !authorization_decision.diagnostics.empty()) {
      const std::string detail = authorization_decision.diagnostics.empty()
                                     ? "SELECT authorization is indeterminate"
                                     : authorization_decision.diagnostics.front().detail;
      return invalid("QOW-DIAG-QRY-004-SCAN-SECURITY-DECISION-V1", detail);
    }
  }

  std::uint64_t maximum_scanned = 0;
  std::uint64_t maximum_bytes = 0;
  std::uint64_t maximum_output = 0;
  if (!CheckedHeapBoundToU64(request.maximum_scanned_row_versions,
                             &maximum_scanned) ||
      !CheckedHeapBoundToU64(maximum_decoded_bytes, &maximum_bytes) ||
      !CheckedHeapBoundToU64(request.maximum_output_rows, &maximum_output)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap scan bound conversion overflowed");
  }
  api::HeapReadRuntimeObservation runtime_observation;
  api::MgaVisibleHeapRelationReadResult read;
  std::optional<CanonicalExactCountStarCardinality>
      exact_count_star_cardinality;
  if (request.exact_global_count_star_consumer) {
    const auto aggregate = std::ranges::find_if(
        relational.nodes, [](const auto& node) {
          return node.node_kind == api::RelationalDagNodeKind::kAggregate;
        });
    const auto physical_aggregate = std::ranges::find_if(
        physical_dag.nodes, [](const auto& node) {
          return node.node_kind == PhysicalNodeKind::kAggregate;
        });
    if (relational.nodes.size() != 2 ||
        physical_dag.nodes.size() != 2 ||
        aggregate == relational.nodes.end() ||
        physical_aggregate == physical_dag.nodes.end() ||
        relational.root_node_id != aggregate->node_id ||
        aggregate->semantic_variant_id !=
            "aggregate.global-count-star.v1" ||
        aggregate->input_node_ids !=
            std::vector<std::uint32_t>{relation_node->node_id} ||
        physical_dag.root_physical_node_id !=
            physical_aggregate->physical_node_id ||
        physical_aggregate->implementation_id !=
            "aggregate.count-star.v1" ||
        physical_aggregate->input_physical_node_ids !=
            std::vector<std::uint64_t>{physical->physical_node_id}) {
      return invalid(
          "QOW-DIAG-QRY-007-AGGREGATE-PHYSICAL-ROUTE-V1",
          "streaming COUNT(*) requires the exact two-node heap aggregate DAG");
    }
    api::MgaVisibleHeapRelationCountRequest count_request;
    count_request.borrowed_relation_uuid = &relation_uuid;
    count_request.maximum_decoded_bytes = maximum_bytes;
    count_request.maximum_memory_bytes = maximum_live_memory_bytes;
    count_request.borrowed_cancellation_requested = &cancellation_requested;
    auto counted = api::CountVisibleMgaHeapRelationWithObservation(
        context, count_request, &runtime_observation,
        prepared_read_authority);
    if (!counted.ok) {
      return invalid(
          counted.diagnostic.code.empty()
              ? "QOW-DIAG-QRY-004-HEAP-READ-V1"
              : counted.diagnostic.code,
          counted.diagnostic.detail,
          counted.scanned_row_version_count != 0 ||
              counted.decoded_byte_count != 0,
          counted.cancellation_observed);
    }
    if (counted.visible_row_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        counted.visible_row_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      return invalid("QOW-DIAG-QRY-007-AGGREGATE-OVERFLOW-V1",
                     "COUNT(*) exceeds the canonical int64 result width",
                     true);
    }
    read.ok = true;
    read.diagnostic = std::move(counted.diagnostic);
    read.descriptor = std::move(counted.descriptor);
    read.current_relation_base_generation =
        counted.current_relation_base_generation;
    read.scanned_row_version_count = counted.scanned_row_version_count;
    read.decoded_byte_count = counted.decoded_byte_count;
    read.visibility_recheck_count = counted.visibility_recheck_count;
    read.invisible_row_version_count = counted.invisible_row_version_count;
    read.tombstone_row_count = counted.tombstone_row_count;
    read.scoped_physical_segment_used =
        counted.scoped_physical_segment_used;
    read.current_live_memory_bytes = counted.current_live_memory_bytes;
    read.peak_live_memory_bytes = counted.peak_live_memory_bytes;
    read.memory_grant_bytes = counted.memory_grant_bytes;
    read.memory_receipt_complete = counted.memory_receipt_complete;
    exact_count_star_cardinality = CanonicalExactCountStarCardinality{
        1,
        counted.visible_row_count,
        counted.scanned_row_version_count,
        counted.decoded_byte_count,
        counted.storage_bytes_read,
        counted.visibility_recheck_count,
        counted.invisible_row_version_count,
        counted.tombstone_row_count,
        true,
        true,
        true};
  } else {
    api::MgaVisibleHeapRelationReadRequest read_request;
    read_request.borrowed_relation_uuid = &relation_uuid;
    read_request.maximum_scanned_row_versions = maximum_scanned;
    read_request.maximum_decoded_bytes = maximum_bytes;
    read_request.maximum_output_rows = maximum_output;
    read_request.maximum_memory_bytes = maximum_live_memory_bytes;
    read_request.borrowed_cancellation_requested = &cancellation_requested;
    read = api::ReadVisibleMgaHeapRelationWithObservation(
        context, read_request, &runtime_observation,
        prepared_read_authority);
  }
  if (!read.ok) {
    return invalid(read.diagnostic.code.empty()
                       ? "QOW-DIAG-QRY-004-HEAP-READ-V1"
                       : read.diagnostic.code,
                   read.diagnostic.detail,
                   read.scanned_row_version_count != 0 ||
                       read.decoded_byte_count != 0,
                   read.cancellation_observed);
  }
  if (!read.memory_receipt_complete ||
      read.memory_grant_bytes != maximum_live_memory_bytes ||
      read.current_live_memory_bytes > read.peak_live_memory_bytes ||
      read.peak_live_memory_bytes > maximum_live_memory_bytes) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap reader did not return a complete bounded memory receipt",
                   true);
  }

  if (read.descriptor.columns.empty() ||
      read.descriptor.columns.size() < output_width ||
      read.descriptor.columns.size() > kMaximumHeapOutputColumns) {
    return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                   "persisted relation width cannot satisfy the projected binding",
                   true);
  }
  if (read.current_live_memory_bytes >= maximum_live_memory_bytes) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap reader retained state exhausts the callback memory allowance",
                   true);
  }
  std::uint64_t materialization_live_bytes =
      read.current_live_memory_bytes + 1;
  std::uint64_t materialization_peak_bytes =
      std::max(read.peak_live_memory_bytes, materialization_live_bytes);
  const auto account_materialization = [&](const std::uint64_t additional) {
    if (additional >
        maximum_live_memory_bytes - materialization_live_bytes) {
      return false;
    }
    materialization_live_bytes += additional;
    materialization_peak_bytes =
        std::max(materialization_peak_bytes, materialization_live_bytes);
    return true;
  };
  std::size_t materialized_cell_count = 0;
  if (!CheckedHeapCellCount(read.visible_rows.size(), output_width,
                            &materialized_cell_count) ||
      materialized_cell_count > request.maximum_output_cells) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap materialized cell bound would be exceeded",
                   true);
  }

  std::uint64_t materialization_structure_bytes = sizeof(DescriptorBatch);
  std::uint64_t allocation_bytes = 0;
  const auto account_array = [&](const std::size_t count,
                                 const std::size_t element_size) {
    return api::HeapReadMemoryMultiply(
               static_cast<std::uint64_t>(count), element_size,
               &allocation_bytes) &&
           api::HeapReadMemoryAdd(
               allocation_bytes, &materialization_structure_bytes);
  };
  if (!account_array(output_width, sizeof(api::EngineDescriptor)) ||
      !account_array(output_width, sizeof(std::string)) ||
      !account_array(output_width,
                     sizeof(const api::MgaRelationColumnStorageDescriptor*)) ||
      !account_array(output_width, sizeof(ExecutorColumnDescriptor)) ||
      !account_array(read.visible_rows.size(), sizeof(DescriptorTuple)) ||
      !account_array(read.visible_rows.size(), sizeof(std::string)) ||
      !account_array(read.visible_rows.size(), sizeof(std::string)) ||
      !account_array(materialized_cell_count,
                     sizeof(api::EngineTypedValue)) ||
      !account_materialization(materialization_structure_bytes)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap output structures exceed the callback memory allowance",
                   true);
  }

  DescriptorBatch batch;
  std::vector<api::EngineDescriptor> output_descriptors;
  std::vector<std::string> column_uuids;
  std::vector<const api::MgaRelationColumnStorageDescriptor*>
      projected_columns;
  for (std::size_t persisted_ordinal = 0;
       persisted_ordinal < read.descriptor.columns.size();
       ++persisted_ordinal) {
    const auto& column = read.descriptor.columns[persisted_ordinal];
    bool duplicate_identity = false;
    for (std::size_t prior = 0; prior < persisted_ordinal; ++prior) {
      const auto& prior_column = read.descriptor.columns[prior];
      if (prior_column.column_uuid.canonical == column.column_uuid.canonical ||
          prior_column.canonical_name_key == column.canonical_name_key) {
        duplicate_identity = true;
        break;
      }
    }
    if (column.ordinal != persisted_ordinal ||
        !IsCanonicalHeapBindingUuid(column.column_uuid.canonical) ||
        column.canonical_name_key.empty() || duplicate_identity) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "persisted relation column identities are incomplete",
                     true);
    }
  }
  output_descriptors.reserve(output_width);
  column_uuids.reserve(output_width);
  projected_columns.reserve(output_width);
  batch.columns.reserve(output_width);
  for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
    const auto output = std::ranges::find_if(
        relational.outputs, [&](const auto& candidate) {
          return candidate.relation_node_id == relation_node->node_id &&
                 candidate.ordinal == ordinal;
        });
    const auto expression = std::ranges::find_if(
        relational.expressions, [&](const auto& candidate) {
          return candidate.expression_id ==
                 relation_node->bound_expression_ids[ordinal];
        });
    const auto relational_descriptor = std::ranges::find_if(
        relational.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id ==
                 relation_node->output_descriptor_ids[ordinal];
        });
    if (output == relational.outputs.end() ||
        expression == relational.expressions.end() ||
        relational_descriptor == relational.descriptors.end()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-BINDING-V1",
                     "projected relation binding disappeared", true);
    }
    const auto persisted_column = std::ranges::find_if(
        read.descriptor.columns, [&](const auto& candidate) {
          return candidate.column_uuid.canonical ==
                 *expression->bound_name_uuid;
        });
    if (persisted_column == read.descriptor.columns.end()) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "projected column UUID is absent from persisted relation",
                     true);
    }
    const auto& column = *persisted_column;
    const auto persisted_type_uuid = ExactHeapDescriptorField(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const bool nullable = relational_descriptor->nullability ==
                          api::RelationalNullability::kNullable;
    if (column.column_uuid.canonical != *expression->bound_name_uuid ||
        output->output_name_utf8 != column.canonical_name_key ||
        column.value_descriptor.descriptor_uuid.canonical !=
            relational_descriptor->descriptor_uuid ||
        column.value_descriptor.encoded_descriptor.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        !persisted_type_uuid.has_value() ||
        !IsCanonicalHeapBindingUuid(*persisted_type_uuid) ||
        *persisted_type_uuid != relational_descriptor->type_uuid ||
        nullable != column.nullable ||
        !ExactHeapNullabilityCarrierMatches(
            column.value_descriptor.encoded_descriptor, nullable) ||
        (relational_descriptor->collation_uuid.has_value()
             ? column.collation_uuid !=
                   *relational_descriptor->collation_uuid
             : !column.collation_uuid.empty()) ||
        !ExactOptionalHeapDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "collation_uuid", relational_descriptor->collation_uuid) ||
        !ExactOptionalHeapDescriptorFieldMatches(
            column.value_descriptor.encoded_descriptor,
            "timezone_profile_id",
            relational_descriptor->timezone_profile_id)) {
      return invalid("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                     "persisted ordinal column descriptor differs from binding",
                     true);
    }
    std::uint64_t projected_descriptor_bytes = 0;
    if (!api::AddHeapReadOwnedStringMemory(column.canonical_name_key,
                                         &projected_descriptor_bytes) ||
        !api::AccountHeapReadEngineDescriptorMemory(
            column.value_descriptor, &projected_descriptor_bytes) ||
        !api::AccountHeapReadEngineDescriptorMemory(
            column.value_descriptor, &projected_descriptor_bytes) ||
        !api::AddHeapReadOwnedStringMemory(column.column_uuid.canonical,
                                         &projected_descriptor_bytes) ||
        !account_materialization(projected_descriptor_bytes)) {
      return invalid(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "heap projected descriptors exceed the callback memory allowance",
          true);
    }
    projected_columns.push_back(&column);
    auto output_descriptor = column.value_descriptor;
    if (output_descriptor.canonical_type_name == "text" &&
        !relational_descriptor->datatype_identity_authoritative) {
      // The persisted column carrier includes storage-only datatype evidence
      // that is not part of a non-contextual relational graph descriptor.
      // Publication at this boundary uses the exact scalar execution view
      // already bound above; contextual descriptors retain the full
      // persisted carrier and are revalidated through their live authority.
      output_descriptor.descriptor_uuid.canonical =
          relational_descriptor->descriptor_uuid;
      output_descriptor.encoded_descriptor =
          "type_uuid=" + relational_descriptor->type_uuid +
          ";nullability=" + (nullable ? "nullable" : "non_null");
      if (relational_descriptor->collation_uuid.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";collation_uuid=" + *relational_descriptor->collation_uuid;
      }
      if (relational_descriptor->width.has_value()) {
        output_descriptor.encoded_descriptor +=
            ";width=" + std::to_string(*relational_descriptor->width);
      }
    } else if (output_descriptor.canonical_type_name == "text" &&
               output_descriptor.encoded_descriptor.find(
                   "datatype_descriptor_uuid=") != std::string::npos &&
               output_descriptor.encoded_descriptor.find("column_uuid=") !=
                   std::string::npos) {
      output_descriptor.descriptor_uuid = column.column_uuid;
    }
    output_descriptor.descriptor_kind = "scalar";
    batch.columns.push_back({column.canonical_name_key,
                             output_descriptor,
                             column.nullable,
                             output->descriptor_id});
    output_descriptors.push_back(std::move(output_descriptor));
    column_uuids.push_back(column.column_uuid.canonical);
  }
  batch.rows.reserve(read.visible_rows.size());
  std::vector<std::string> record_uuids;
  std::vector<std::string> version_uuids;
  record_uuids.reserve(read.visible_rows.size());
  version_uuids.reserve(read.visible_rows.size());
  for (std::size_t row_index = 0; row_index < read.visible_rows.size();
       ++row_index) {
    if (cancellation_requested()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
                     "heap relation acquisition cancelled during materialization",
                     true,
                     true);
    }
    const auto& stored_row = read.visible_rows[row_index];
    if (stored_row.values.size() != read.descriptor.columns.size()) {
      return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                     "stored row width differs from persisted descriptor width",
                     true);
    }
    DescriptorTuple tuple;
    tuple.values.reserve(output_width);
    for (std::size_t ordinal = 0; ordinal < output_width; ++ordinal) {
      const auto& column = *projected_columns[ordinal];
      const std::string* encoded_value = nullptr;
      for (const auto& [name, value] : stored_row.values) {
        if (name != column.canonical_name_key) { continue; }
        if (encoded_value != nullptr) {
          return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                         "stored row contains a duplicate persisted column",
                         true);
        }
        encoded_value = &value;
      }
      if (encoded_value == nullptr) {
        return invalid("QOW-DIAG-QRY-004-HEAP-VALUE-V1",
                       "stored row omits a persisted column",
                       true);
      }
      api::EngineTypedValue value;
      std::uint64_t value_descriptor_bytes = 0;
      if (!api::AccountHeapReadEngineDescriptorMemory(
              output_descriptors[ordinal], &value_descriptor_bytes) ||
          !account_materialization(value_descriptor_bytes)) {
        return invalid(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "heap value descriptors exceed the callback memory allowance",
            true);
      }
      value.descriptor = output_descriptors[ordinal];
      if (*encoded_value == "<NULL>") {
        value.is_null = true;
        value.state = api::EngineValueState::sql_null;
      } else {
        std::uint64_t encoded_allocation_bytes = 0;
        if (!api::AddHeapReadOwnedStringMemory(
                *encoded_value, &encoded_allocation_bytes) ||
            !account_materialization(encoded_allocation_bytes)) {
          return invalid(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "heap value materialization exceeds the callback memory allowance",
              true);
        }
        value.encoded_value = *encoded_value;
        value.state = api::EngineValueState::value;
      }
      tuple.values.push_back(std::move(value));
    }
    std::uint64_t identity_allocation_bytes = 0;
    if (!api::AddHeapReadOwnedStringMemory(
            stored_row.row_uuid, &identity_allocation_bytes) ||
        !api::AddHeapReadOwnedStringMemory(
            stored_row.version_uuid, &identity_allocation_bytes) ||
        !account_materialization(identity_allocation_bytes)) {
      return invalid(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "heap row identity materialization exceeds the callback memory allowance",
          true);
    }
    batch.rows.push_back(std::move(tuple));
    record_uuids.push_back(stored_row.row_uuid);
    version_uuids.push_back(stored_row.version_uuid);
  }
  bool validation_bound_cancellation_observed = false;
  bool validation_bound_probe_failed = false;
  const auto validation_scratch =
      BoundDescriptorBatchValidationScratchMemoryBytes(
          batch, cancellation_requested,
          &validation_bound_cancellation_observed,
          &validation_bound_probe_failed);
  if (validation_bound_probe_failed) {
    return invalid(
        "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
        "heap descriptor-validation cancellation probe threw",
        true);
  }
  if (validation_bound_cancellation_observed) {
    return invalid("QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
                   "heap relation acquisition cancelled before validation",
                   true, true);
  }
  if (!validation_scratch.has_value() ||
      *validation_scratch >
          maximum_live_memory_bytes - materialization_live_bytes) {
    return invalid(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "heap descriptor-validation scratch exceeds the callback memory allowance",
        true);
  }
  materialization_peak_bytes = std::max(
      materialization_peak_bytes,
      materialization_live_bytes + *validation_scratch);
  bool validation_cancellation_observed = false;
  const auto batch_validation = ValidateCanonicalDescriptorBatch(
      batch, physical->output_descriptor_ids, cancellation_requested,
      &validation_cancellation_observed);
  if (!batch_validation.ok) {
    return invalid(batch_validation.diagnostic_code,
                   batch_validation.detail,
                   true, validation_cancellation_observed);
  }
  const auto value_validation = ValidateDescriptorBatch(
      batch, cancellation_requested, &validation_cancellation_observed);
  if (!value_validation.ok) {
    return invalid(value_validation.diagnostic_code,
                   value_validation.detail,
                   true, validation_cancellation_observed);
  }
  if (prepared_read_authority == nullptr) {
    const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!result_authority.ok) return refuse(result_authority, true);
  }

  // These fields are copied into the returned acquisition receipt after the
  // materialized batch is frozen. Charge their exact dynamic carriers before
  // publishing the final current/peak values.
  std::uint64_t result_metadata_bytes = 0;
  std::uint64_t result_metadata_array_bytes = 0;
  const auto account_result_string = [&](const std::string& value) {
    return api::AddHeapReadOwnedStringMemory(value, &result_metadata_bytes);
  };
  const auto account_result_u64_vector =
      [&](const std::vector<std::uint64_t>& values) {
        return api::HeapReadMemoryMultiply(
                   static_cast<std::uint64_t>(values.size()),
                   sizeof(std::uint64_t), &result_metadata_array_bytes) &&
               api::HeapReadMemoryAdd(result_metadata_array_bytes,
                                             &result_metadata_bytes);
      };
  const auto& result_mga_context = mga_authority.statement_context;
  if (!account_result_string(relation_uuid) ||
      !account_result_string(read.descriptor.descriptor_uuid.canonical) ||
      !account_result_string(physical_dag.selected_plan_uuid) ||
      !account_result_string(result_mga_context.statement_uuid) ||
      !account_result_string(result_mga_context.owning_transaction_uuid) ||
      !account_result_string(result_mga_context.statement_snapshot_uuid) ||
      !account_result_string(
          result_mga_context.statement_metadata_snapshot_uuid) ||
      !account_result_u64_vector(
          result_mga_context.active_excluded_local_transaction_ids) ||
      !account_result_u64_vector(
          result_mga_context.in_doubt_excluded_local_transaction_ids) ||
      !account_result_string(result_mga_context.snapshot_kind) ||
      !account_result_string(result_mga_context.statement_timestamp) ||
      !account_materialization(result_metadata_bytes)) {
    return invalid("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "heap result metadata exceeds the callback memory allowance",
                   true);
  }

  result.diagnostic = {};
  result.output_batch = std::move(batch);
  result.emitted_record_uuids = std::move(record_uuids);
  result.emitted_row_version_uuids = std::move(version_uuids);
  result.counters.scanned_row_version_count =
      read.scanned_row_version_count;
  result.counters.decoded_byte_count = read.decoded_byte_count;
  result.runtime_operator_wait_ns = runtime_observation.operator_wait_ns;
  result.runtime_storage_bytes_read =
      runtime_observation.storage_bytes_read;
  result.runtime_decoded_bytes = runtime_observation.decoded_bytes;
  result.runtime_current_memory_bytes =
      materialization_live_bytes - read.current_live_memory_bytes;
  result.runtime_peak_memory_bytes = materialization_peak_bytes;
  result.runtime_memory_grant_bytes = maximum_live_memory_bytes;
  result.runtime_memory_receipt_complete =
      result.runtime_current_memory_bytes <= result.runtime_peak_memory_bytes &&
      result.runtime_peak_memory_bytes <= result.runtime_memory_grant_bytes;
  result.runtime_observation_complete = runtime_observation.complete;
  result.counters.visibility_recheck_count = read.visibility_recheck_count;
  result.counters.invisible_row_version_count =
      read.invisible_row_version_count;
  result.counters.tombstone_row_count = read.tombstone_row_count;
  result.counters.emitted_row_count =
      exact_count_star_cardinality.has_value()
          ? static_cast<std::size_t>(
                exact_count_star_cardinality->visible_row_count)
          : result.output_batch.rows.size();
  result.counters.output_column_count = output_width;
  result.counters.materialized_cell_count = materialized_cell_count;
  result.authority.engine_catalog_descriptor_loaded = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.engine_authorization_rechecked = true;
  result.authority.bounded_physical_read = true;
  result.data_access_observed = read.scoped_physical_segment_used;
  result.relation_uuid = relation_uuid;
  result.column_uuids = std::move(column_uuids);
  result.current_relation_descriptor_uuid =
      read.descriptor.descriptor_uuid.canonical;
  result.current_relation_descriptor_generation =
      read.descriptor.descriptor_generation;
  result.selected_plan_uuid = physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = physical->physical_node_id;
  result.causal_counter_id = physical->causal_counter_id;
  result.mga_statement_context = mga_authority.statement_context;
  result.exact_count_star_cardinality =
      std::move(exact_count_star_cardinality);
  return result;
}

CanonicalHeapRelationAcquisitionResult ExecuteCanonicalHeapRelationAcquisition(
    const CanonicalHeapRelationAcquisitionRequest& request) {
  return ExecuteCanonicalHeapRelationAcquisitionPrepared(request, nullptr);
}

namespace {

struct HeapPhysicalDispatchObservation {
  bool callback_invoked = false;
  bool data_access_observed = false;
  bool cancellation_observed = false;
};

struct HeapAcquisitionLeafBinding {
  std::uint64_t physical_node_id{0};
  std::shared_ptr<const
      scratchbird::engine::internal_api::PreparedMgaHeapReadAuthority>
      read_authority;
};

struct HeapPhysicalExecutorState {
  std::optional<scratchbird::engine::internal_api::EngineRequestContext>
      owned_context;
  std::optional<scratchbird::engine::internal_api::TypedRelationalDag>
      owned_relational_dag;
  std::optional<TypedPhysicalNodeDag> owned_physical_dag;
  const scratchbird::engine::internal_api::EngineRequestContext* context =
      nullptr;
  const scratchbird::engine::internal_api::TypedRelationalDag* relational_dag =
      nullptr;
  const TypedPhysicalNodeDag* physical_dag = nullptr;
  std::size_t maximum_scanned_row_versions = 0;
  std::size_t maximum_decoded_bytes = 0;
  std::size_t maximum_output_rows = 0;
  std::size_t maximum_output_columns = 0;
  std::size_t maximum_output_cells = 0;
  std::function<bool()> owned_cancellation_requested;
  const std::function<bool()>* cancellation_requested = nullptr;
  std::optional<CanonicalExecutionMgaAuthority> owned_mga_authority;
  std::shared_ptr<const CanonicalExecutionMgaAuthority> shared_mga_authority;
  const CanonicalExecutionMgaAuthority* mga_authority = nullptr;
  std::shared_ptr<const scratchbird::engine::internal_api::
                            PreparedMgaHeapReadAuthorityCohort>
      heap_read_authority_cohort;
  std::optional<CanonicalHeapTableSampleProfile> table_sample_profile;
  bool exact_global_count_star_consumer = false;
  std::vector<HeapAcquisitionLeafBinding> acquisition_leaf_bindings;
};

bool ExactGlobalCountStarHeapConsumer(
    const scratchbird::engine::internal_api::TypedRelationalDag& relational,
    const TypedPhysicalNodeDag& physical,
    const PhysicalNodeRecord& heap_node) {
  if (relational.nodes.size() != 2 || physical.nodes.size() != 2 ||
      heap_node.node_kind != PhysicalNodeKind::kScan ||
      heap_node.implementation_id != "scan.heap.v1" ||
      !heap_node.input_physical_node_ids.empty()) {
    return false;
  }
  const auto scan = std::ranges::find_if(
      relational.nodes, [&](const auto& node) {
        return node.node_id == heap_node.relational_node_id;
      });
  const auto aggregate = std::ranges::find_if(
      relational.nodes, [](const auto& node) {
        return node.node_kind ==
               scratchbird::engine::internal_api::RelationalDagNodeKind::
                   kAggregate;
      });
  const auto physical_aggregate = std::ranges::find_if(
      physical.nodes, [](const auto& node) {
        return node.node_kind == PhysicalNodeKind::kAggregate;
      });
  return scan != relational.nodes.end() &&
         aggregate != relational.nodes.end() &&
         physical_aggregate != physical.nodes.end() &&
         scan->node_kind ==
             scratchbird::engine::internal_api::RelationalDagNodeKind::kScan &&
         scan->semantic_variant_id == "relation.source.v1" &&
         relational.root_node_id == aggregate->node_id &&
         aggregate->semantic_variant_id ==
             "aggregate.global-count-star.v1" &&
         aggregate->input_node_ids ==
             std::vector<std::uint32_t>{scan->node_id} &&
         physical.root_physical_node_id ==
             physical_aggregate->physical_node_id &&
         physical_aggregate->implementation_id ==
             "aggregate.count-star.v1" &&
         physical_aggregate->input_physical_node_ids ==
             std::vector<std::uint64_t>{heap_node.physical_node_id};
}

bool AccountHeapRegistrationString(const std::string& value,
                                   std::uint64_t* bytes) {
  return scratchbird::engine::internal_api::AddHeapReadOwnedStringMemory(
      value, bytes);
}

bool AccountHeapRegistrationDescriptor(
    const scratchbird::engine::internal_api::MgaRelationStorageDescriptor&
        descriptor,
    std::uint64_t* bytes) {
  namespace api = scratchbird::engine::internal_api;
  std::uint64_t allocation_bytes = 0;
  const auto account_uuid = [&](const api::EngineUuid& uuid) {
    return AccountHeapRegistrationString(uuid.canonical, bytes);
  };
  if (!account_uuid(descriptor.descriptor_uuid) ||
      !account_uuid(descriptor.database_uuid) ||
      !account_uuid(descriptor.schema_uuid) ||
      !account_uuid(descriptor.relation_uuid) ||
      !account_uuid(descriptor.primary_filespace_uuid) ||
      !AccountHeapRegistrationString(descriptor.relation_kind, bytes) ||
      !AccountHeapRegistrationString(descriptor.storage_profile, bytes) ||
      !AccountHeapRegistrationString(descriptor.row_identity_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.version_identity_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.mutation_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.visibility_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.cleanup_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.recovery_rule, bytes) ||
      !AccountHeapRegistrationString(descriptor.descriptor_status, bytes) ||
      !api::HeapReadMemoryMultiply(
          descriptor.columns.capacity(),
          sizeof(api::MgaRelationColumnStorageDescriptor),
          &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes) ||
      !api::HeapReadMemoryMultiply(
          descriptor.indexes.capacity(),
          sizeof(api::MgaRelationIndexStorageDescriptor),
          &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes) ||
      !api::HeapReadMemoryMultiply(
          descriptor.required_evidence_kinds.capacity(), sizeof(std::string),
          &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes)) {
    return false;
  }
  for (const auto& column : descriptor.columns) {
    if (!account_uuid(column.column_uuid) ||
        !AccountHeapRegistrationString(column.canonical_name_key, bytes) ||
        !api::AccountHeapReadEngineDescriptorMemory(column.value_descriptor,
                                                    bytes) ||
        !AccountHeapRegistrationString(column.storage_class, bytes) ||
        !AccountHeapRegistrationString(column.charset_uuid, bytes) ||
        !AccountHeapRegistrationString(column.collation_uuid, bytes) ||
        !AccountHeapRegistrationString(column.overflow_policy, bytes)) {
      return false;
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!account_uuid(index.index_uuid) ||
        !AccountHeapRegistrationString(index.family, bytes) ||
        !AccountHeapRegistrationString(index.profile, bytes) ||
        !AccountHeapRegistrationString(index.predicate_kind, bytes) ||
        !AccountHeapRegistrationString(index.predicate_column, bytes) ||
        !AccountHeapRegistrationString(index.predicate_value, bytes) ||
        !AccountHeapRegistrationString(index.residency_policy, bytes)) {
      return false;
    }
    for (const auto* values : {&index.key_envelopes,
                               &index.include_columns}) {
      if (!api::HeapReadMemoryMultiply(
              values->capacity(), sizeof(std::string), &allocation_bytes) ||
          !api::HeapReadMemoryAdd(allocation_bytes, bytes)) {
        return false;
      }
      for (const auto& value : *values) {
        if (!AccountHeapRegistrationString(value, bytes)) return false;
      }
    }
  }
  for (const auto& evidence : descriptor.required_evidence_kinds) {
    if (!AccountHeapRegistrationString(evidence, bytes)) return false;
  }
  return true;
}

bool AccountHeapRegistrationMgaContext(
    const PhysicalMgaStatementContext& context,
    std::uint64_t* bytes) {
  namespace api = scratchbird::engine::internal_api;
  std::uint64_t allocation_bytes = 0;
  return AccountHeapRegistrationString(context.statement_uuid, bytes) &&
         AccountHeapRegistrationString(context.owning_transaction_uuid,
                                       bytes) &&
         AccountHeapRegistrationString(context.statement_snapshot_uuid,
                                       bytes) &&
         AccountHeapRegistrationString(
             context.statement_metadata_snapshot_uuid, bytes) &&
         api::HeapReadMemoryMultiply(
             context.active_excluded_local_transaction_ids.capacity(),
             sizeof(std::uint64_t), &allocation_bytes) &&
         api::HeapReadMemoryAdd(allocation_bytes, bytes) &&
         api::HeapReadMemoryMultiply(
             context.in_doubt_excluded_local_transaction_ids.capacity(),
             sizeof(std::uint64_t), &allocation_bytes) &&
         api::HeapReadMemoryAdd(allocation_bytes, bytes) &&
         AccountHeapRegistrationString(context.snapshot_kind, bytes) &&
         AccountHeapRegistrationString(context.statement_timestamp, bytes);
}

std::optional<std::uint64_t> HeapPhysicalRegistrationRetainedMemoryBytes(
    const HeapPhysicalExecutorState& state,
    const HeapPhysicalDispatchObservation& observation) {
  namespace api = scratchbird::engine::internal_api;
  constexpr std::uint64_t kTreeNodeOverhead = 4 * sizeof(void*);
  constexpr std::uint64_t kSharedAllocationOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(state) + sizeof(observation) +
                        3 * sizeof(std::shared_ptr<void>) +
                        3 * kSharedAllocationOverhead;
  std::uint64_t allocation_bytes = 0;
  if (state.mga_authority == nullptr ||
      !AccountHeapRegistrationMgaContext(
          state.mga_authority->statement_context, &bytes) ||
      !api::HeapReadMemoryAdd(
          sizeof(CanonicalExecutionMgaAuthority) +
              kSharedAllocationOverhead,
          &bytes) ||
      !api::HeapReadMemoryAdd(
          sizeof(CurrentHeapMgaResolutionBinding) +
              kSharedAllocationOverhead,
          &bytes) ||
      !AccountHeapRegistrationString(state.context->database_path, &bytes) ||
      !AccountHeapRegistrationString(
          state.context->transaction_uuid.canonical, &bytes) ||
      !AccountHeapRegistrationString(
          state.context->statement_uuid.canonical, &bytes) ||
      !AccountHeapRegistrationString(
          state.context->statement_snapshot_uuid.canonical, &bytes) ||
      !AccountHeapRegistrationString(
          state.context->statement_metadata_snapshot_uuid.canonical, &bytes) ||
      !api::HeapReadMemoryMultiply(
          state.acquisition_leaf_bindings.capacity(),
          sizeof(HeapAcquisitionLeafBinding), &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (std::size_t binding_index = 0;
       binding_index < state.acquisition_leaf_bindings.size();
       ++binding_index) {
    const auto& binding = state.acquisition_leaf_bindings[binding_index];
    const auto* authority = binding.read_authority.get();
    bool already_accounted = false;
    for (std::size_t prior = 0; prior < binding_index; ++prior) {
      if (state.acquisition_leaf_bindings[prior].read_authority.get() ==
          authority) {
        already_accounted = true;
        break;
      }
    }
    if (authority == nullptr || already_accounted) {
      continue;
    }
    const auto* statement = authority->statement.get();
    if (statement == nullptr || statement->transaction_states == nullptr) {
      return std::nullopt;
    }
    bool statement_already_accounted = false;
    for (std::size_t prior = 0; prior < binding_index; ++prior) {
      const auto* prior_authority =
          state.acquisition_leaf_bindings[prior].read_authority.get();
      if (prior_authority != nullptr &&
          prior_authority->statement.get() == statement) {
        statement_already_accounted = true;
        break;
      }
    }
    if (!api::HeapReadMemoryAdd(
            sizeof(api::PreparedMgaHeapReadAuthority) +
                kSharedAllocationOverhead,
            &bytes) ||
        !AccountHeapRegistrationDescriptor(authority->descriptor, &bytes) ||
        !AccountHeapRegistrationString(authority->relation_uuid, &bytes)) {
      return std::nullopt;
    }
    if (statement_already_accounted) {
      continue;
    }
    if (!api::HeapReadMemoryAdd(
            sizeof(api::PreparedMgaHeapStatementAuthority) +
                kSharedAllocationOverhead,
            &bytes) ||
        !api::HeapReadMemoryMultiply(
            statement->snapshot_vector
                .active_excluded_local_transaction_ids.capacity(),
            sizeof(std::uint64_t), &allocation_bytes) ||
        !api::HeapReadMemoryAdd(allocation_bytes, &bytes) ||
        !api::HeapReadMemoryMultiply(
            statement->snapshot_vector
                .in_doubt_excluded_local_transaction_ids.capacity(),
            sizeof(std::uint64_t), &allocation_bytes) ||
        !api::HeapReadMemoryAdd(allocation_bytes, &bytes) ||
        !api::HeapReadMemoryMultiply(
            statement->transaction_states->size(),
            sizeof(std::pair<const std::uint64_t, std::string>) +
                kTreeNodeOverhead,
            &allocation_bytes) ||
        !api::HeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& [transaction_id, state_name] :
         *statement->transaction_states) {
      (void)transaction_id;
      if (!AccountHeapRegistrationString(state_name, &bytes)) {
        return std::nullopt;
      }
    }
    for (const auto* value : {
             &statement->database_uuid,
             &statement->statement_uuid,
             &statement->transaction_uuid,
             &statement->statement_snapshot_uuid,
             &statement->statement_metadata_snapshot_uuid,
             &statement->catalog_epoch_uuid,
             &statement->authorization_authority_uuid,
             &statement->metadata_path,
             &statement->savepoint_path,
             &statement->descriptor_path}) {
      if (!AccountHeapRegistrationString(*value, &bytes)) {
        return std::nullopt;
      }
    }
  }
  return bytes;
}

bool HeapRuntimeBatchLiveBytes(const DescriptorBatch& batch,
                               std::uint64_t* bytes) {
  namespace api = scratchbird::engine::internal_api;
  if (bytes == nullptr) return false;
  *bytes = sizeof(DescriptorBatch);
  std::uint64_t allocation_bytes = 0;
  if (!api::HeapReadMemoryMultiply(
          static_cast<std::uint64_t>(batch.columns.capacity()),
          sizeof(ExecutorColumnDescriptor), &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes) ||
      !api::HeapReadMemoryMultiply(
          static_cast<std::uint64_t>(batch.rows.capacity()),
          sizeof(DescriptorTuple), &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes)) {
    return false;
  }
  for (const auto& column : batch.columns) {
    if (!api::AddHeapReadOwnedStringMemory(column.stable_name, bytes) ||
        !api::AccountHeapReadEngineDescriptorMemory(column.descriptor,
                                                    bytes)) {
      return false;
    }
  }
  for (const auto& row : batch.rows) {
    if (!api::HeapReadMemoryMultiply(
            static_cast<std::uint64_t>(row.values.capacity()),
            sizeof(api::EngineTypedValue), &allocation_bytes) ||
        !api::HeapReadMemoryAdd(allocation_bytes, bytes)) {
      return false;
    }
    for (const auto& value : row.values) {
      if (!api::AccountHeapReadEngineDescriptorMemory(value.descriptor,
                                                      bytes) ||
          !api::AddHeapReadOwnedStringMemory(value.encoded_value, bytes) ||
          !api::HeapReadMemoryAdd(
              static_cast<std::uint64_t>(value.binary_value.capacity()),
              bytes)) {
        return false;
      }
    }
  }
  return true;
}

bool HeapRuntimeStringVectorLiveBytes(
    const std::vector<std::string>& values,
    std::uint64_t* bytes) {
  namespace api = scratchbird::engine::internal_api;
  if (bytes == nullptr) return false;
  *bytes = sizeof(values);
  std::uint64_t allocation_bytes = 0;
  if (!api::HeapReadMemoryMultiply(
          static_cast<std::uint64_t>(values.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !api::HeapReadMemoryAdd(allocation_bytes, bytes)) {
    return false;
  }
  for (const auto& value : values) {
    if (!api::AddHeapReadOwnedStringMemory(value, bytes)) {
      return false;
    }
  }
  return true;
}

bool HeapRuntimeMgaContextDynamicBytes(
    const PhysicalMgaStatementContext& context,
    std::uint64_t* bytes) {
  namespace api = scratchbird::engine::internal_api;
  std::uint64_t allocation_bytes = 0;
  return bytes != nullptr &&
         api::AddHeapReadOwnedStringMemory(context.statement_uuid, bytes) &&
         api::AddHeapReadOwnedStringMemory(context.owning_transaction_uuid,
                                         bytes) &&
         api::AddHeapReadOwnedStringMemory(context.statement_snapshot_uuid,
                                         bytes) &&
         api::AddHeapReadOwnedStringMemory(
             context.statement_metadata_snapshot_uuid, bytes) &&
         api::HeapReadMemoryMultiply(
             static_cast<std::uint64_t>(
                 context.active_excluded_local_transaction_ids.capacity()),
             sizeof(std::uint64_t), &allocation_bytes) &&
         api::HeapReadMemoryAdd(allocation_bytes, bytes) &&
         api::HeapReadMemoryMultiply(
             static_cast<std::uint64_t>(
                 context.in_doubt_excluded_local_transaction_ids.capacity()),
             sizeof(std::uint64_t), &allocation_bytes) &&
         api::HeapReadMemoryAdd(allocation_bytes, bytes) &&
         api::AddHeapReadOwnedStringMemory(context.snapshot_kind, bytes) &&
         api::AddHeapReadOwnedStringMemory(context.statement_timestamp, bytes);
}

struct HeapPhysicalRegistrationBuildResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::optional<CanonicalPhysicalExecutorRegistration> registration;
  std::shared_ptr<HeapPhysicalDispatchObservation> observation;
};

bool SameHeapPhysicalNodeIdentity(const PhysicalNodeRecord& left,
                                  const PhysicalNodeRecord& right) {
  return left.physical_node_id == right.physical_node_id &&
         left.relational_node_id == right.relational_node_id &&
         left.node_kind == right.node_kind &&
         left.implementation_id == right.implementation_id &&
         left.input_physical_node_ids == right.input_physical_node_ids &&
         left.output_descriptor_ids == right.output_descriptor_ids &&
         left.causal_counter_id == right.causal_counter_id &&
         left.selected_alternative_uuid == right.selected_alternative_uuid &&
         left.executor_capability_uuid == right.executor_capability_uuid &&
         left.executor_capability_abi_version ==
             right.executor_capability_abi_version &&
         left.engine_capability_validated ==
             right.engine_capability_validated;
}

bool SameHeapPhysicalDagAuthority(const TypedPhysicalNodeDag& left,
                                  const TypedPhysicalNodeDag& right) {
  return left.abi_version == right.abi_version &&
         left.selected_plan_uuid == right.selected_plan_uuid &&
         left.root_physical_node_id == right.root_physical_node_id &&
         left.local_transaction_id == right.local_transaction_id &&
         left.statement_snapshot_id == right.statement_snapshot_id &&
         PhysicalMgaStatementContextEqual(left.mga_statement_context,
                                          right.mga_statement_context) &&
         left.bound_sblr_tree_uuid == right.bound_sblr_tree_uuid &&
         left.catalog_epoch_uuid == right.catalog_epoch_uuid &&
         left.security_context_uuid == right.security_context_uuid &&
         left.capability_snapshot_uuid == right.capability_snapshot_uuid &&
         left.resource_snapshot_uuid == right.resource_snapshot_uuid &&
         left.statistics_snapshot_uuid == right.statistics_snapshot_uuid &&
         left.route_snapshot_uuid == right.route_snapshot_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.security_epoch == right.security_epoch &&
         left.policy_epoch == right.policy_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.statistics_generation == right.statistics_generation &&
         left.route_epoch == right.route_epoch &&
         left.route_generation == right.route_generation &&
         left.memory_budget_bytes == right.memory_budget_bytes &&
         left.optimizer_published == right.optimizer_published &&
         left.immutable_node_identity_validated ==
             right.immutable_node_identity_validated &&
         left.capability_validated_before_access ==
             right.capability_validated_before_access &&
         left.data_access_observed == right.data_access_observed &&
         left.nodes.size() == right.nodes.size();
}

std::uint64_t HeapPhysicalResultHandle(const TypedPhysicalNodeDag& dag,
                                       const PhysicalNodeRecord& node) {
  std::uint64_t value = 1469598103934665603ULL;
  const auto mix_byte = [&](const std::uint8_t byte) {
    value ^= byte;
    value *= 1099511628211ULL;
  };
  for (const unsigned char ch : dag.selected_plan_uuid) { mix_byte(ch); }
  for (const unsigned char ch : node.executor_capability_uuid) { mix_byte(ch); }
  for (std::size_t offset = 0; offset < sizeof(node.physical_node_id);
       ++offset) {
    mix_byte(static_cast<std::uint8_t>(
        node.physical_node_id >> (offset * 8u)));
  }
  return value == 0 ? 1 : value;
}

std::string_view HeapTableSampleImplementationId(
    const CanonicalHeapTableSampleProfile& profile) {
  return profile.method == CanonicalHeapTableSampleMethod::kBernoulli
             ? "scan.heap.tablesample.bernoulli.v1"
             : "scan.heap.tablesample.system.v1";
}

std::string_view HeapTableSampleSemanticId(
    const CanonicalHeapTableSampleProfile& profile) {
  return profile.method == CanonicalHeapTableSampleMethod::kBernoulli
             ? "relation.source.tablesample.bernoulli.v1"
             : "relation.source.tablesample.system.v1";
}

scratchbird::engine::internal_api::CanonicalSeededSampleRequest
HeapSeededSampleRequest(const CanonicalHeapTableSampleProfile& profile,
                        const std::size_t input_row_count,
                        const std::size_t maximum_input_row_count) {
  namespace api = scratchbird::engine::internal_api;
  api::CanonicalSeededSampleRequest request;
  request.input_row_count = input_row_count;
  request.method =
      profile.method == CanonicalHeapTableSampleMethod::kBernoulli
          ? api::CanonicalSeededSampleMethod::kBernoulli
          : api::CanonicalSeededSampleMethod::kSystem;
  request.sample_basis_points = profile.sample_basis_points;
  request.repeatable_seed = profile.repeatable_seed;
  request.repeatable_seed_is_bound = profile.repeatable_seed_is_bound;
  request.system_block_row_count = profile.system_block_row_count;
  request.maximum_input_row_count = maximum_input_row_count;
  return request;
}

DescriptorRuntimeDiagnostic ValidateHeapTableSampleProfile(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  const auto& physical = request.borrowed_physical_dag == nullptr
                             ? request.physical_dag
                             : *request.borrowed_physical_dag;
  if (!request.table_sample_profile.has_value()) return {};
  const auto& profile = *request.table_sample_profile;
  if (profile.method != CanonicalHeapTableSampleMethod::kBernoulli &&
      profile.method != CanonicalHeapTableSampleMethod::kSystem) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE method is outside the accepted profile");
  }
  if (request.relational_dag == nullptr ||
      request.relational_dag->nodes.size() != 1 ||
      physical.nodes.size() != 1) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE requires one optimizer-selected heap scan");
  }
  if (profile.predicate_placement ==
      CanonicalHeapTableSamplePredicatePlacement::kBeforeSample) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-PUSHDOWN-V1",
        "predicate pushdown below TABLESAMPLE changes the sample population");
  }
  if (profile.predicate_placement !=
          CanonicalHeapTableSamplePredicatePlacement::kAbsent &&
      profile.predicate_placement !=
          CanonicalHeapTableSamplePredicatePlacement::kAfterSample) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-PUSHDOWN-V1",
        "TABLESAMPLE predicate placement is outside the accepted profile");
  }
  const auto seeded = HeapSeededSampleRequest(profile, 0, 1);
  const auto descriptor_uuid =
      api::CanonicalSeededSampleDescriptorUuid(seeded);
  const auto& relational_node = request.relational_dag->nodes.front();
  const auto& physical_node = physical.nodes.front();
  if (descriptor_uuid.empty() ||
      relational_node.semantic_variant_id !=
          HeapTableSampleSemanticId(profile) ||
      physical_node.implementation_id !=
          HeapTableSampleImplementationId(profile)) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-HEAP-PROFILE-V1",
        "TABLESAMPLE method, seed, rate, or scan profile is unresolved");
  }
  if (physical_node.selected_alternative_uuid != descriptor_uuid) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-015-SEED-IDENTITY-V1",
        "TABLESAMPLE seed descriptor is absent from selected-plan identity");
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateHeapPhysicalRegistrationRequest(
    const CanonicalHeapPhysicalDagDispatchRequest& request,
    std::vector<const PhysicalNodeRecord*>* heap_nodes) {
  namespace api = scratchbird::engine::internal_api;
  if (heap_nodes == nullptr || request.context == nullptr ||
      request.relational_dag == nullptr) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-BINDING-V1",
        "engine context and typed relational DAG are required");
  }
  const auto& context = *request.context;
  const auto& relational = *request.relational_dag;
  const auto& physical = request.borrowed_physical_dag == nullptr
                             ? request.physical_dag
                             : *request.borrowed_physical_dag;
  const auto& mga_authority = request.borrowed_mga_authority == nullptr
                                  ? request.mga_authority
                                  : *request.borrowed_mga_authority;
  const auto& cancellation_requested =
      request.borrowed_cancellation_requested == nullptr
          ? request.cancellation_requested
          : *request.borrowed_cancellation_requested;
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(context.database_path);
  if (mga_authority.origin !=
      CanonicalMgaAuthorityOrigin::kEngineTransactionInventory) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
        "heap registration requires engine transaction-inventory authority");
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      mga_authority, physical);
  if (!entry_authority.ok) return entry_authority;
  const auto relational_validation = api::ValidateTypedRelationalDag(relational);
  if (!relational_validation.accepted) {
    const auto& issue = relational_validation.issues.front();
    return HeapAcquisitionRefusal(issue.diagnostic_id, issue.field_id);
  }
  const auto physical_validation =
      ValidateTypedPhysicalNodeDag(physical);
  if (!physical_validation.accepted) {
    const auto& issue = physical_validation.issues.front();
    return HeapAcquisitionRefusal(issue.diagnostic_id, issue.field_id);
  }
  const auto current_mga =
      ValidateCurrentHeapPhysicalMgaAuthority(context, physical);
  if (!current_mga.ok) {
    return current_mga;
  }
  if (relational.wire_version != 2 ||
      physical.abi_version != 2 ||
      !physical.optimizer_published ||
      !physical.immutable_node_identity_validated ||
      !physical.capability_validated_before_access ||
      physical.data_access_observed) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-SCOPE-V1",
        "wire-v2 optimizer-published physical authority is required");
  }
  if (!cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > kMaximumHeapOutputColumns) {
    return HeapAcquisitionRefusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "valid nonzero heap dispatch shape bounds and cancellation probe are "
        "required");
  }
  if (cancellation_requested()) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-CANCELLED-V1",
        "heap physical dispatch cancelled before registration");
  }
  const auto& authorization = context.authorization_context;
  if (context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      physical.local_transaction_id !=
          context.local_transaction_id ||
      physical.statement_snapshot_id !=
          context.snapshot_visible_through_local_transaction_id ||
      !context.statement_metadata_snapshot_engine_owned ||
      !context.security_context_present || !authorization.present ||
      relational.bound_sblr_tree_uuid !=
          physical.bound_sblr_tree_uuid ||
      relational.statement_uuid != context.statement_uuid.canonical ||
      relational.owning_transaction_uuid !=
          context.transaction_uuid.canonical ||
      relational.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      relational.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      relational.local_transaction_id != context.local_transaction_id ||
      relational.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id ||
      relational.bound_catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      physical.catalog_epoch_uuid !=
          context.catalog_epoch_uuid.canonical ||
      relational.bound_security_context_uuid !=
          authorization.authority_uuid.canonical ||
      physical.security_context_uuid !=
          authorization.authority_uuid.canonical ||
      physical.catalog_generation !=
          context.catalog_generation_id ||
      context.catalog_generation_id !=
          authorization.catalog_generation_id ||
      physical.security_epoch != context.security_epoch ||
      context.security_epoch != authorization.security_epoch ||
      physical.policy_epoch != authorization.policy_epoch ||
      physical.resource_epoch != context.resource_epoch ||
      physical.capability_snapshot_uuid !=
          context.optimizer_capability_snapshot_uuid.canonical ||
      physical.resource_snapshot_uuid !=
          context.optimizer_resource_snapshot_uuid.canonical ||
      physical.route_snapshot_uuid !=
          context.optimizer_route_snapshot_uuid.canonical ||
      physical.route_epoch != context.optimizer_route_epoch ||
      physical.route_generation !=
          context.optimizer_route_generation ||
      physical.memory_budget_bytes !=
          context.optimizer_memory_budget_bytes ||
      context.optimizer_memory_budget_bytes == 0 ||
      context.optimizer_maximum_candidate_count == 0 ||
      request.maximum_decoded_bytes > context.optimizer_memory_budget_bytes ||
      request.maximum_scanned_row_versions >
          context.optimizer_maximum_candidate_count ||
      request.maximum_output_rows >
          context.optimizer_maximum_candidate_count) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-AUTHORITY-V1",
        "heap registration authority is stale, missing, or over budget");
  }
  if (!context.prepared_metadata_required_object_uuid.canonical.empty() ||
      context.prepared_metadata_required_executable_generation != 0 ||
      context.prepared_metadata_required_metadata_epoch != 0) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-SCOPE-V1",
        "prepared or cached descriptor dispatch is outside this profile");
  }
  const auto sample_validation = ValidateHeapTableSampleProfile(request);
  if (!sample_validation.ok) return sample_validation;

  heap_nodes->clear();
  const std::string_view expected_implementation =
      request.table_sample_profile.has_value()
          ? HeapTableSampleImplementationId(*request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  std::string capability_uuid;
  std::uint32_t capability_abi = 0;
  for (const auto& node : physical.nodes) {
    if (node.implementation_id != expected_implementation) { continue; }
    if (node.node_kind != PhysicalNodeKind::kScan ||
        !node.input_physical_node_ids.empty() ||
        node.output_descriptor_ids.empty() ||
        node.output_descriptor_ids.size() > request.maximum_output_columns ||
        node.output_descriptor_ids.size() > kMaximumHeapOutputColumns ||
        node.executor_capability_uuid.empty() ||
        node.executor_capability_abi_version == 0 ||
        !node.engine_capability_validated) {
      return HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
          "heap executor capability is incomplete");
    }
    if (heap_nodes->empty()) {
      capability_uuid = node.executor_capability_uuid;
      capability_abi = node.executor_capability_abi_version;
    } else if (node.executor_capability_uuid != capability_uuid ||
               node.executor_capability_abi_version != capability_abi) {
      return HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
          "heap executor capability identity is inconsistent");
    }
    heap_nodes->push_back(&node);
  }
  if (heap_nodes->empty()) {
    return HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
        "the bound heap scan implementation is not present in the selected "
        "physical DAG");
  }
  return {};
}

HeapPhysicalRegistrationBuildResult BuildHeapPhysicalRegistration(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  HeapPhysicalRegistrationBuildResult result;
  std::vector<const PhysicalNodeRecord*> heap_nodes;
  result.diagnostic =
      ValidateHeapPhysicalRegistrationRequest(request, &heap_nodes);
  if (!result.diagnostic.ok) { return result; }
  const auto& physical = request.borrowed_physical_dag == nullptr
                             ? request.physical_dag
                             : *request.borrowed_physical_dag;
  const auto& mga_authority = request.borrowed_mga_authority == nullptr
                                  ? request.mga_authority
                                  : *request.borrowed_mga_authority;

  auto state = std::make_shared<HeapPhysicalExecutorState>();
  if (request.borrowed_physical_dag != nullptr) {
    state->context = request.context;
    state->relational_dag = request.relational_dag;
    state->physical_dag = &physical;
  } else {
    state->owned_context = *request.context;
    state->owned_relational_dag = *request.relational_dag;
    state->owned_physical_dag = physical;
    state->context = &*state->owned_context;
    state->relational_dag = &*state->owned_relational_dag;
    state->physical_dag = &*state->owned_physical_dag;
  }
  state->maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  state->maximum_decoded_bytes = request.maximum_decoded_bytes;
  state->maximum_output_rows = request.maximum_output_rows;
  state->maximum_output_columns = request.maximum_output_columns;
  state->maximum_output_cells = request.maximum_output_cells;
  if (request.borrowed_cancellation_requested != nullptr) {
    state->cancellation_requested = request.borrowed_cancellation_requested;
  } else {
    state->owned_cancellation_requested = request.cancellation_requested;
    state->cancellation_requested = &state->owned_cancellation_requested;
  }
  if (request.shared_mga_authority != nullptr) {
    state->shared_mga_authority = request.shared_mga_authority;
    state->mga_authority = state->shared_mga_authority.get();
  } else {
    state->owned_mga_authority = mga_authority;
    state->mga_authority = &*state->owned_mga_authority;
  }
  state->table_sample_profile = request.table_sample_profile;
  state->exact_global_count_star_consumer =
      heap_nodes.size() == 1 && !state->table_sample_profile.has_value() &&
      ExactGlobalCountStarHeapConsumer(
          *state->relational_dag, *state->physical_dag,
          *heap_nodes.front());

  std::vector<std::string> relation_uuids;
  relation_uuids.reserve(heap_nodes.size());
  for (const auto* heap_node : heap_nodes) {
    const auto prepared_relation_node = std::ranges::find_if(
        state->relational_dag->nodes, [&](const auto& candidate) {
          return candidate.node_id == heap_node->relational_node_id;
        });
    if (prepared_relation_node == state->relational_dag->nodes.end() ||
        prepared_relation_node->required_object_uuids.size() != 1) {
      result.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
          "heap acquisition leaf has no exact relation identity");
      return result;
    }
    const auto& relation_uuid =
        prepared_relation_node->required_object_uuids.front();
    if (std::ranges::find(relation_uuids, relation_uuid) ==
        relation_uuids.end()) {
      relation_uuids.push_back(relation_uuid);
    }
  }
  state->heap_read_authority_cohort = request.heap_read_authority_cohort;
  if (state->heap_read_authority_cohort == nullptr) {
    auto prepared =
        scratchbird::engine::internal_api::PrepareMgaHeapReadAuthorities(
            *state->context, relation_uuids);
    if (!prepared.ok || prepared.cohort == nullptr) {
      result.diagnostic = HeapAcquisitionRefusal(
          prepared.diagnostic.code.empty()
              ? "QOW-DIAG-QRY-004-HEAP-READ-PREPARATION-V1"
              : prepared.diagnostic.code,
          prepared.diagnostic.detail);
      return result;
    }
    state->heap_read_authority_cohort = std::move(prepared.cohort);
  }
  if (state->heap_read_authority_cohort->relations.size() !=
      relation_uuids.size()) {
    result.diagnostic = HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-READ-PREPARATION-V1",
        "heap authority cohort differs from the exact relation set");
    return result;
  }
  const auto authority_fence = scratchbird::engine::internal_api::
      RevalidatePreparedMgaHeapReadAuthorityCohort(
          *state->context, *state->heap_read_authority_cohort);
  if (authority_fence.error) {
    result.diagnostic = HeapAcquisitionRefusal(
        authority_fence.code.empty()
            ? "QOW-DIAG-QRY-004-HEAP-READ-PREPARATION-V1"
            : authority_fence.code,
        authority_fence.detail);
    return result;
  }
  state->acquisition_leaf_bindings.reserve(heap_nodes.size());
  for (const auto* heap_node : heap_nodes) {
    HeapAcquisitionLeafBinding binding;
    binding.physical_node_id = heap_node->physical_node_id;
    const auto prepared_relation_node = std::ranges::find_if(
        state->relational_dag->nodes, [&](const auto& candidate) {
          return candidate.node_id == heap_node->relational_node_id;
        });
    if (prepared_relation_node == state->relational_dag->nodes.end()) {
      result.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
          "heap acquisition leaf is unresolved in the relational DAG");
      return result;
    }
    if (prepared_relation_node->required_object_uuids.size() != 1) {
      result.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
          "heap acquisition leaf has no exact relation identity");
      return result;
    }
    const auto& relation_uuid =
        prepared_relation_node->required_object_uuids.front();
    const auto authority =
        state->heap_read_authority_cohort->relations.find(relation_uuid);
    if (authority == state->heap_read_authority_cohort->relations.end() ||
        authority->second == nullptr) {
      result.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-HEAP-READ-PREPARATION-V1",
          "heap relation is absent from the exact authority cohort");
      return result;
    }
    binding.read_authority = authority->second;
    state->acquisition_leaf_bindings.push_back(std::move(binding));
  }
  result.observation = std::make_shared<HeapPhysicalDispatchObservation>();

  CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = heap_nodes.front()->node_kind;
  registration.implementation_id = heap_nodes.front()->implementation_id;
  registration.executor_capability_uuid =
      heap_nodes.front()->executor_capability_uuid;
  registration.executor_capability_abi_version =
      heap_nodes.front()->executor_capability_abi_version;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      !state->table_sample_profile.has_value();
  const auto retained_memory =
      HeapPhysicalRegistrationRetainedMemoryBytes(*state, *result.observation);
  if (!retained_memory.has_value() || *retained_memory == 0) {
    result.diagnostic = HeapAcquisitionRefusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "heap executor retained-memory receipt is absent or overflows");
    return result;
  }
  registration.retained_live_memory_bytes_v1 = *retained_memory;
  registration.execute =
      [state, observation = result.observation](
          const TypedPhysicalNodeDag& dispatched_dag,
          const PhysicalNodeRecord& dispatched_node,
          const std::vector<CanonicalPhysicalDispatchInput>& inputs) {
        CanonicalPhysicalDispatchStepResult step;
        observation->callback_invoked = true;
        // QOW-SOURCE-QRY-004-DATA-ACCESS-OBSERVATION-V1
        // This engine-owned callback always knows whether the bounded heap
        // acquisition crossed its physical-read boundary, including refusal
        // returns.
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        if (!inputs.empty()) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-INPUT-V1",
              "leaf heap executor does not accept input batches");
          return step;
        }
        const auto callback_memory_limit = std::min(
            dispatched_node.memory_bytes_required,
            dispatched_node.dispatcher_callback_memory_limit_bytes);
        if (dispatched_node.dispatcher_callback_memory_limit_bytes == 0 ||
            callback_memory_limit == 0 ||
            callback_memory_limit > dispatched_dag.memory_budget_bytes ||
            callback_memory_limit >
                std::numeric_limits<std::size_t>::max()) {
          step.diagnostic = HeapAcquisitionRefusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "heap executor callback memory allowance is absent or invalid");
          return step;
        }
        const auto expected_node = std::ranges::find_if(
            state->physical_dag->nodes, [&](const auto& candidate) {
              return candidate.physical_node_id ==
                     dispatched_node.physical_node_id;
            });
        if (!SameHeapPhysicalDagAuthority(dispatched_dag,
                                          *state->physical_dag) ||
            expected_node == state->physical_dag->nodes.end() ||
            !SameHeapPhysicalNodeIdentity(dispatched_node, *expected_node) ||
            dispatched_node.node_kind != PhysicalNodeKind::kScan ||
            dispatched_node.implementation_id !=
                (state->table_sample_profile.has_value()
                     ? HeapTableSampleImplementationId(
                           *state->table_sample_profile)
                     : std::string_view{"scan.heap.v1"}) ||
            !dispatched_node.input_physical_node_ids.empty()) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
              "dispatched heap node differs from captured optimizer authority");
          return step;
        }
        const auto acquisition_binding = std::ranges::find_if(
            state->acquisition_leaf_bindings, [&](const auto& candidate) {
              return candidate.physical_node_id ==
                     dispatched_node.physical_node_id;
            });
        if (acquisition_binding == state->acquisition_leaf_bindings.end() ||
            acquisition_binding->read_authority == nullptr) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-IDENTITY-V1",
              "heap acquisition leaf is unresolved in the relational DAG");
          return step;
        }
        CanonicalHeapRelationAcquisitionRequest acquisition_request;
        acquisition_request.context = state->context;
        acquisition_request.relational_dag = state->relational_dag;
        acquisition_request.borrowed_physical_dag = state->physical_dag;
        acquisition_request.selected_physical_node_id =
            dispatched_node.physical_node_id;
        acquisition_request.maximum_scanned_row_versions =
            state->maximum_scanned_row_versions;
        acquisition_request.maximum_decoded_bytes =
            std::min<std::size_t>(state->maximum_decoded_bytes,
                                  callback_memory_limit);
        acquisition_request.maximum_output_rows =
            state->table_sample_profile.has_value()
                ? state->maximum_scanned_row_versions
                : state->maximum_output_rows;
        acquisition_request.maximum_output_columns =
            state->maximum_output_columns;
        acquisition_request.maximum_output_cells = state->maximum_output_cells;
        acquisition_request.maximum_live_memory_bytes = callback_memory_limit;
        if (state->table_sample_profile.has_value() &&
            !CheckedHeapCellCount(
                state->maximum_scanned_row_versions,
                dispatched_node.output_descriptor_ids.size(),
                &acquisition_request.maximum_output_cells)) {
          step.diagnostic = HeapAcquisitionRefusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "TABLESAMPLE visible-population cell bound overflows size_t");
          return step;
        }
        acquisition_request.borrowed_cancellation_requested =
            state->cancellation_requested;
        acquisition_request.borrowed_mga_authority = state->mga_authority;
        acquisition_request.exact_global_count_star_consumer =
            state->exact_global_count_star_consumer;
        auto acquisition = ExecuteCanonicalHeapRelationAcquisitionPrepared(
            acquisition_request, acquisition_binding->read_authority.get());
        observation->data_access_observed = acquisition.data_access_observed;
        observation->cancellation_observed = acquisition.cancellation_observed;
        step.data_access_observed = acquisition.data_access_observed;
        if (!acquisition.diagnostic.ok) {
          step.diagnostic = std::move(acquisition.diagnostic);
          if (acquisition.cancellation_observed) {
            const PhysicalAdmissionEvidence* cancellation_policy = nullptr;
            bool duplicate_policy = false;
            for (const auto& evidence : dispatched_dag.admission_evidence) {
              if (evidence.stage !=
                  PhysicalAdmissionStage::kPolicyCapability) {
                continue;
              }
              if (cancellation_policy != nullptr) {
                duplicate_policy = true;
                break;
              }
              cancellation_policy = &evidence;
            }
            if (cancellation_policy == nullptr || duplicate_policy) {
              step.diagnostic = HeapAcquisitionRefusal(
                  "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-RECEIPT-V1",
                  "heap cancellation policy identity is ambiguous");
              return step;
            }
            step.selected_plan_uuid = dispatched_dag.selected_plan_uuid;
            step.executed_physical_node_id =
                dispatched_node.physical_node_id;
            step.causal_counter_id = dispatched_node.causal_counter_id;
            step.output_descriptor_ids =
                dispatched_node.output_descriptor_ids;
            step.authority.engine_mga_snapshot_bound = true;
            step.mga_statement_context =
                state->mga_authority->statement_context;
            step.cancellation_observed = true;
            step.transient_state_cleanup_proven = true;
            step.cancellation_evidence_uuid =
                cancellation_policy->evidence_uuid;
          }
          return step;
        }
        if (!acquisition.runtime_observation_complete ||
            !acquisition.runtime_memory_receipt_complete ||
            acquisition.runtime_memory_grant_bytes != callback_memory_limit ||
            acquisition.runtime_current_memory_bytes >
                acquisition.runtime_peak_memory_bytes ||
            acquisition.runtime_peak_memory_bytes > callback_memory_limit) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-OPT-017-REFUSAL-V1",
              "heap_runtime_observation_or_memory_receipt_incomplete");
          return step;
        }
        if (!PhysicalMgaStatementContextEqual(
                acquisition.mga_statement_context,
                state->mga_authority->statement_context)) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-MGA-VECTOR-V1",
              "heap acquisition returned a different MGA statement context");
          return step;
        }
        const std::uint64_t pre_sample_resident_bytes =
            acquisition.runtime_current_memory_bytes;
        if (pre_sample_resident_bytes == 0 ||
            pre_sample_resident_bytes > callback_memory_limit) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-OPT-017-REFUSAL-V1",
              "heap_runtime_observation_resident_invalid");
          return step;
        }
        std::uint64_t sample_auxiliary_bytes = 0;

        std::optional<CanonicalHeapTableSampleActuals> sample_actuals;
        if (state->table_sample_profile.has_value()) {
          namespace api = scratchbird::engine::internal_api;
          const auto visible_input_row_count =
              acquisition.output_batch.rows.size();
          if (acquisition.emitted_record_uuids.size() !=
                  visible_input_row_count ||
              acquisition.emitted_row_version_uuids.size() !=
                  visible_input_row_count) {
            step.diagnostic = HeapAcquisitionRefusal(
                "QOW-DIAG-QRY-015-VISIBILITY-CARRIER-V1",
                "TABLESAMPLE input lost visible record/version identity");
            return step;
          }
          auto sample_request = HeapSeededSampleRequest(
              *state->table_sample_profile, visible_input_row_count,
              state->maximum_scanned_row_versions);
          std::uint64_t maximum_sample_batch_bytes = 0;
          std::uint64_t maximum_sample_record_bytes = 0;
          std::uint64_t maximum_sample_version_bytes = 0;
          std::uint64_t maximum_sample_index_bytes = 0;
          std::uint64_t maximum_sampling_live_bytes =
              pre_sample_resident_bytes;
          bool sample_preflight_ok =
              HeapRuntimeBatchLiveBytes(acquisition.output_batch,
                                        &maximum_sample_batch_bytes) &&
              HeapRuntimeStringVectorLiveBytes(
                  acquisition.emitted_record_uuids,
                  &maximum_sample_record_bytes) &&
              HeapRuntimeStringVectorLiveBytes(
                  acquisition.emitted_row_version_uuids,
                  &maximum_sample_version_bytes) &&
              api::HeapReadMemoryMultiply(
                  static_cast<std::uint64_t>(visible_input_row_count),
                  sizeof(std::size_t), &maximum_sample_index_bytes) &&
              api::HeapReadMemoryAdd(
                  maximum_sample_batch_bytes,
                  &maximum_sampling_live_bytes) &&
              api::HeapReadMemoryAdd(
                  maximum_sample_record_bytes,
                  &maximum_sampling_live_bytes) &&
              api::HeapReadMemoryAdd(
                  maximum_sample_version_bytes,
                  &maximum_sampling_live_bytes) &&
              api::HeapReadMemoryAdd(
                  maximum_sample_index_bytes,
                  &maximum_sampling_live_bytes) &&
              // Fixed formatter/method/actuals envelope; UUID output itself
              // is exactly 36 bytes and both method identifiers are fixed.
              api::HeapReadMemoryAdd(2048,
                                            &maximum_sampling_live_bytes);
          if (!sample_preflight_ok ||
              maximum_sampling_live_bytes > callback_memory_limit) {
            step.diagnostic = HeapAcquisitionRefusal(
                "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                "TABLESAMPLE transient output exceeds the callback memory allowance");
            return step;
          }
          const auto sampled = api::ExecuteCanonicalSeededSample(
              sample_request);
          if (!sampled.accepted) {
            step.diagnostic = HeapAcquisitionRefusal(
                sampled.diagnostic_code, sampled.detail);
            return step;
          }
          if (sampled.selected_row_indices.capacity() >
              std::numeric_limits<std::uint64_t>::max() /
                  sizeof(std::size_t)) {
            step.diagnostic = HeapAcquisitionRefusal(
                "QOW-DIAG-OPT-017-REFUSAL-V1",
                "heap_runtime_observation_sample_memory_overflow");
            return step;
          }
          sample_auxiliary_bytes =
              sampled.selected_row_indices.capacity() * sizeof(std::size_t);
          std::size_t sampled_output_cell_count = 0;
          if (sampled.selected_row_indices.size() >
                  state->maximum_output_rows ||
              !CheckedHeapCellCount(
                  sampled.selected_row_indices.size(),
                  acquisition.output_batch.columns.size(),
                  &sampled_output_cell_count) ||
              sampled_output_cell_count > state->maximum_output_cells) {
            step.diagnostic = HeapAcquisitionRefusal(
                "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                "TABLESAMPLE output exceeds the admitted result bound");
            return step;
          }
          decltype(acquisition.output_batch.rows) rows;
          std::vector<std::string> record_uuids;
          std::vector<std::string> version_uuids;
          rows.reserve(sampled.selected_row_indices.size());
          record_uuids.reserve(sampled.selected_row_indices.size());
          version_uuids.reserve(sampled.selected_row_indices.size());
          for (const auto row : sampled.selected_row_indices) {
            if (row >= visible_input_row_count) {
              step.diagnostic = HeapAcquisitionRefusal(
                  "QOW-DIAG-QRY-015-SAMPLE-INDEX-V1",
                  "TABLESAMPLE selected an out-of-range visible row");
              return step;
            }
            rows.push_back(acquisition.output_batch.rows[row]);
            record_uuids.push_back(acquisition.emitted_record_uuids[row]);
            version_uuids.push_back(
                acquisition.emitted_row_version_uuids[row]);
          }
          acquisition.output_batch.rows = std::move(rows);
          acquisition.emitted_record_uuids = std::move(record_uuids);
          acquisition.emitted_row_version_uuids = std::move(version_uuids);
          acquisition.counters.emitted_row_count =
              acquisition.output_batch.rows.size();
          acquisition.counters.materialized_cell_count =
              sampled_output_cell_count;
          CanonicalHeapTableSampleActuals actuals;
          actuals.sample_descriptor_uuid =
              api::CanonicalSeededSampleDescriptorUuid(sample_request);
          actuals.method_id = sampled.method_id;
          actuals.sample_basis_points = sample_request.sample_basis_points;
          actuals.visible_input_row_count = visible_input_row_count;
          actuals.examined_unit_count = sampled.examined_unit_count;
          actuals.sampled_output_row_count =
              sampled.selected_row_indices.size();
          actuals.repeatable_seed_bound =
              sample_request.repeatable_seed_is_bound;
          actuals.sampling_applied_after_mga_visibility = true;
          actuals.predicate_pushdown_legality_validated = true;
          sample_actuals = std::move(actuals);
        }

        // Admit all step-owned dynamic carriers before copying them. The
        // acquisition receipt remains live until the callback returns, so the
        // prospective step metadata must fit beside that complete resident
        // result rather than being checked only after allocation.
        namespace api = scratchbird::engine::internal_api;
        std::uint64_t prospective_step_metadata_bytes = 0;
        std::uint64_t prospective_step_array_bytes = 0;
        const auto account_prospective_string = [&](const std::string& value) {
          return api::AddHeapReadOwnedStringMemory(
              value, &prospective_step_metadata_bytes);
        };
        bool prospective_step_bounded =
            account_prospective_string(acquisition.selected_plan_uuid) &&
            api::HeapReadMemoryMultiply(
                static_cast<std::uint64_t>(
                    dispatched_node.output_descriptor_ids.size()),
                sizeof(std::uint32_t), &prospective_step_array_bytes) &&
            api::HeapReadMemoryAdd(
                prospective_step_array_bytes,
                &prospective_step_metadata_bytes) &&
            account_prospective_string(
                acquisition.current_relation_descriptor_uuid) &&
            HeapRuntimeMgaContextDynamicBytes(
                state->mga_authority->statement_context,
                &prospective_step_metadata_bytes);
        if (prospective_step_bounded && sample_actuals.has_value()) {
          prospective_step_bounded =
              account_prospective_string(
                  sample_actuals->sample_descriptor_uuid) &&
              account_prospective_string(sample_actuals->method_id);
        }
        std::uint64_t prospective_callback_live_bytes =
            acquisition.runtime_current_memory_bytes;
        prospective_step_bounded =
            prospective_step_bounded &&
            api::HeapReadMemoryAdd(
                prospective_step_metadata_bytes,
                &prospective_callback_live_bytes);
        if (!prospective_step_bounded ||
            prospective_callback_live_bytes > callback_memory_limit) {
          step.diagnostic = HeapAcquisitionRefusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "heap step metadata exceeds the callback memory allowance");
          return step;
        }

        step.diagnostic = {};
        step.selected_plan_uuid = acquisition.selected_plan_uuid;
        step.executed_physical_node_id =
            acquisition.executed_physical_node_id;
        step.causal_counter_id = acquisition.causal_counter_id;
        step.result_handle_id =
            HeapPhysicalResultHandle(dispatched_dag, dispatched_node);
        step.output_descriptor_ids = dispatched_node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound =
            acquisition.authority.engine_mga_snapshot_bound;
        step.output_row_count = acquisition.counters.emitted_row_count;
        step.rows_examined = acquisition.counters.scanned_row_version_count;
        if (sample_actuals.has_value()) {
          step.table_sample_actuals = std::move(sample_actuals);
        }
        step.heap_read_counters = acquisition.counters;
        step.heap_read_authority = acquisition.authority;
        step.current_relation_descriptor_uuid =
            acquisition.current_relation_descriptor_uuid;
        step.current_relation_descriptor_generation =
            acquisition.current_relation_descriptor_generation;
        step.exact_count_star_cardinality =
            acquisition.exact_count_star_cardinality;
        step.mga_statement_context = state->mga_authority->statement_context;
        const auto observed = [](const std::uint64_t value) {
          return CanonicalObservedUint64{
              CanonicalRuntimeMetricState::kObserved, value};
        };
        const auto not_applicable = [] {
          return CanonicalObservedUint64{
              CanonicalRuntimeMetricState::kNotApplicable, 0};
        };
        std::uint64_t output_batch_live_bytes = 0;
        std::uint64_t step_metadata_bytes = 0;
        std::uint64_t allocation_bytes = 0;
        const auto account_step_string = [&](const std::string& value) {
          return api::AddHeapReadOwnedStringMemory(value,
                                                 &step_metadata_bytes);
        };
        bool memory_accounting_ok =
            HeapRuntimeBatchLiveBytes(acquisition.output_batch,
                                      &output_batch_live_bytes) &&
            account_step_string(step.selected_plan_uuid) &&
            api::HeapReadMemoryMultiply(
                static_cast<std::uint64_t>(
                    step.output_descriptor_ids.capacity()),
                sizeof(std::uint32_t), &allocation_bytes) &&
            api::HeapReadMemoryAdd(allocation_bytes,
                                          &step_metadata_bytes) &&
            account_step_string(step.current_relation_descriptor_uuid) &&
            HeapRuntimeMgaContextDynamicBytes(step.mga_statement_context,
                                              &step_metadata_bytes);
        if (memory_accounting_ok && step.table_sample_actuals.has_value()) {
          memory_accounting_ok =
              account_step_string(
                  step.table_sample_actuals->sample_descriptor_uuid) &&
              account_step_string(step.table_sample_actuals->method_id);
        }
        std::uint64_t current_memory_bytes = output_batch_live_bytes;
        memory_accounting_ok =
            memory_accounting_ok &&
            api::HeapReadMemoryAdd(step_metadata_bytes,
                                          &current_memory_bytes);
        std::uint64_t peak_memory_bytes =
            acquisition.runtime_peak_memory_bytes;
        std::uint64_t callback_coexistence_bytes =
            acquisition.runtime_current_memory_bytes;
        memory_accounting_ok =
            memory_accounting_ok &&
            api::HeapReadMemoryAdd(step_metadata_bytes,
                                          &callback_coexistence_bytes);
        std::uint64_t sampled_transient_bytes = 0;
        if (memory_accounting_ok &&
            state->table_sample_profile.has_value()) {
          std::uint64_t sampled_records_bytes = 0;
          std::uint64_t sampled_versions_bytes = 0;
          memory_accounting_ok =
              HeapRuntimeStringVectorLiveBytes(
                  acquisition.emitted_record_uuids,
                  &sampled_records_bytes) &&
              HeapRuntimeStringVectorLiveBytes(
                  acquisition.emitted_row_version_uuids,
                  &sampled_versions_bytes) &&
              api::HeapReadMemoryAdd(output_batch_live_bytes,
                                            &sampled_transient_bytes) &&
              api::HeapReadMemoryAdd(sampled_records_bytes,
                                            &sampled_transient_bytes) &&
              api::HeapReadMemoryAdd(sampled_versions_bytes,
                                            &sampled_transient_bytes);
        }
        if (!memory_accounting_ok) {
          step.diagnostic = HeapAcquisitionRefusal(
              "QOW-DIAG-OPT-017-REFUSAL-V1",
              "heap_runtime_observation_memory_overflow");
          return step;
        }
        if (state->table_sample_profile.has_value()) {
          // The source visible batch, the growing sampled batch, and the
          // sampled identity/index vectors coexist until source replacement.
          std::uint64_t sampling_peak_bytes = pre_sample_resident_bytes;
          if (!api::HeapReadMemoryAdd(sampled_transient_bytes,
                                             &sampling_peak_bytes) ||
              !api::HeapReadMemoryAdd(sample_auxiliary_bytes,
                                             &sampling_peak_bytes) ||
              !api::HeapReadMemoryAdd(step_metadata_bytes,
                                             &sampling_peak_bytes)) {
            step.diagnostic = HeapAcquisitionRefusal(
                "QOW-DIAG-OPT-017-REFUSAL-V1",
                "heap_runtime_observation_memory_overflow");
            return step;
          }
          peak_memory_bytes =
              std::max(peak_memory_bytes, sampling_peak_bytes);
        }
        peak_memory_bytes = std::max(
            peak_memory_bytes,
            std::max(current_memory_bytes, callback_coexistence_bytes));
        if (peak_memory_bytes > callback_memory_limit) {
          step.diagnostic = HeapAcquisitionRefusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "heap runtime peak exceeds the callback memory allowance");
          return step;
        }
        auto& runtime = step.runtime_observation;
        runtime.abi_version = 1;
        runtime.operator_wait_ns =
            observed(acquisition.runtime_operator_wait_ns);
        runtime.current_memory_bytes = observed(current_memory_bytes);
        runtime.peak_memory_bytes = observed(peak_memory_bytes);
        runtime.decoded_bytes =
            observed(acquisition.runtime_decoded_bytes);
        runtime.bytes_read =
            observed(acquisition.runtime_storage_bytes_read);
        runtime.bytes_written = not_applicable();
        runtime.pages_read = not_applicable();
        runtime.pages_written = not_applicable();
        runtime.spill_bytes_read = observed(0);
        runtime.spill_bytes_written = observed(0);
        runtime.visibility_recheck_count =
            observed(acquisition.counters.visibility_recheck_count);
        runtime.security_recheck_count =
            acquisition.authority.engine_authorization_rechecked
                ? observed(1)
                : CanonicalObservedUint64{};
        runtime.storage_recheck_count =
            acquisition.authority.engine_catalog_descriptor_loaded
                ? observed(1)
                : CanonicalObservedUint64{};
        runtime.index_recheck_count = not_applicable();
        runtime.residual_recheck_count = not_applicable();
        runtime.compatibility_recheck_count = not_applicable();
        runtime.archive_bytes_read = not_applicable();
        runtime.cluster_bytes_sent = not_applicable();
        runtime.cluster_bytes_received = not_applicable();
        runtime.authority.engine_execution_observation = true;
        runtime.producer_receipt_complete = true;
        step.materialized_output_batch = std::move(acquisition.output_batch);
        return step;
      };
  result.registration = std::move(registration);
  return result;
}

CanonicalPhysicalDagDispatchResult HeapPhysicalDispatchRefusal(
    DescriptorRuntimeDiagnostic diagnostic,
    const bool execution_started = false,
    const bool data_access_observed = false) {
  CanonicalPhysicalDagDispatchResult result;
  result.diagnostic = std::move(diagnostic);
  result.execution_started = execution_started;
  result.data_access_observed = data_access_observed;
  return result;
}

}  // namespace

CanonicalHeapPhysicalRegistrationResult
BuildCanonicalHeapPhysicalRegistration(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  CanonicalHeapPhysicalRegistrationResult result;
  if (request.context == nullptr) {
    result.diagnostic = HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-REGISTRATION-V1",
        "engine context is required");
    return result;
  }
  const auto& physical = request.borrowed_physical_dag == nullptr
                             ? request.physical_dag
                             : *request.borrowed_physical_dag;
  auto authority = std::make_shared<const CanonicalExecutionMgaAuthority>(
      BuildCurrentHeapExecutionMgaAuthority(*request.context, physical));
  auto owned = request;
  owned.shared_mga_authority = authority;
  owned.borrowed_mga_authority = authority.get();
  auto built = BuildHeapPhysicalRegistration(owned);
  result.diagnostic = std::move(built.diagnostic);
  result.registration = std::move(built.registration);
  result.mga_authority = std::move(authority);
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-DISPATCH-V1
CanonicalPhysicalDagDispatchResult ExecuteCanonicalHeapPhysicalDagDispatch(
    const CanonicalHeapPhysicalDagDispatchRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  if (request.context == nullptr) {
    return HeapPhysicalDispatchRefusal(HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-BINDING-V1",
        "engine context is required"));
  }
  const auto inventory_guard =
      api::AcquireTransactionInventoryGuard(request.context->database_path);
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!entry_authority.ok) {
    return HeapPhysicalDispatchRefusal(entry_authority);
  }
  auto built = BuildHeapPhysicalRegistration(request);
  if (!built.diagnostic.ok || !built.registration.has_value() ||
      built.observation == nullptr) {
    if (built.diagnostic.ok) {
      built.diagnostic = HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
          "engine heap executor registration is unavailable");
    }
    return HeapPhysicalDispatchRefusal(std::move(built.diagnostic));
  }
  const auto& physical = request.physical_dag;
  const std::string_view expected_implementation =
      request.table_sample_profile.has_value()
          ? HeapTableSampleImplementationId(*request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  if (physical.nodes.size() != 1 ||
      physical.root_physical_node_id !=
          physical.nodes.front().physical_node_id ||
      physical.nodes.front().node_kind != PhysicalNodeKind::kScan ||
      physical.nodes.front().implementation_id != expected_implementation ||
      !physical.nodes.front().input_physical_node_ids.empty()) {
    return HeapPhysicalDispatchRefusal(HeapAcquisitionRefusal(
        "QOW-DIAG-QRY-004-HEAP-DISPATCH-ROOT-V1",
        "exactly one input-free bound heap scan root is required"));
  }

  CanonicalPhysicalDagDispatchRequest dispatch_request;
  dispatch_request.physical_dag = physical;
  dispatch_request.mga_authority = request.mga_authority;
  dispatch_request.available_executors.push_back(
      std::move(*built.registration));
  auto dispatched = ExecuteCanonicalPhysicalDag(dispatch_request);
  dispatched.execution_started = built.observation->callback_invoked;
  dispatched.data_access_observed =
      built.observation->data_access_observed;
  if (!dispatched.diagnostic.ok) {
    if (!dispatched.executed_steps.empty() ||
        dispatched.root_result_handle_id != 0 ||
        !dispatched.root_output_descriptor_ids.empty()) {
      return HeapPhysicalDispatchRefusal(
          HeapAcquisitionRefusal(
              "QOW-DIAG-QRY-004-HEAP-DISPATCH-ATOMICITY-V1",
              "failed heap dispatch exposed a partial result"),
          built.observation->callback_invoked,
          built.observation->data_access_observed);
    }
    return dispatched;
  }
  if (!built.observation->callback_invoked ||
      dispatched.executed_steps.size() != 1 ||
      dispatched.root_result_handle_id == 0 ||
      dispatched.root_output_descriptor_ids !=
          physical.nodes.front().output_descriptor_ids ||
      dispatched.executed_root_physical_node_id !=
          physical.nodes.front().physical_node_id ||
      dispatched.root_causal_counter_id !=
          physical.nodes.front().causal_counter_id ||
      !dispatched.executed_steps.front().materialized_output_batch.has_value() ||
      !dispatched.executed_steps.front().heap_read_counters.has_value() ||
      !dispatched.executed_steps.front().heap_read_authority.has_value() ||
      !PhysicalMgaStatementContextEqual(
          dispatched.mga_statement_context,
          request.mga_authority.statement_context) ||
      !PhysicalMgaStatementContextEqual(
          dispatched.executed_steps.front().mga_statement_context,
          request.mga_authority.statement_context)) {
    return HeapPhysicalDispatchRefusal(
        HeapAcquisitionRefusal(
            "QOW-DIAG-QRY-004-HEAP-DISPATCH-EVIDENCE-V1",
            "heap dispatch lost materialized or causal evidence"),
        built.observation->callback_invoked,
        built.observation->data_access_observed);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return HeapPhysicalDispatchRefusal(
        result_authority, true, built.observation->data_access_observed);
  }
  return dispatched;
}

}  // namespace scratchbird::engine::executor

namespace scratchbird::engine::internal_api {

// QOW-SOURCE-QRY-004-HEAP-RESULT-V1
// Closes the one-leaf heap profile by deriving all executor and publication
// bindings from admitted engine-owned authority and then entering the single
// canonical optimizer-selected execution spine.
CanonicalOptimizerSelectedExecutionResult
ExecuteCanonicalHeapOptimizerSelectedDag(
    const CanonicalHeapOptimizerSelectedExecutionRequest& request) {
  namespace exec = scratchbird::engine::executor;
  const auto refuse = [](std::string diagnostic_id,
                         std::uint64_t physical_node_id,
                         std::string field_id) {
    CanonicalOptimizerSelectedExecutionResult result;
    result.issues.push_back({std::move(diagnostic_id), physical_node_id,
                             std::move(field_id)});
    return result;
  };

  const auto& context = request.context;
  const auto& relational = request.relational_dag;
  const auto& physical = request.selected_physical_dag;
  const auto inventory_guard =
      AcquireTransactionInventoryGuard(context.database_path);
  if (!exec::IsCanonicalHeapBindingUuid(context.statement_uuid.canonical) ||
      !exec::IsCanonicalHeapBindingUuid(request.execution_attempt_uuid) ||
      !exec::IsCanonicalHeapBindingUuid(
          request.transaction_effect_evidence_uuid) ||
      context.statement_uuid.canonical == request.execution_attempt_uuid ||
      context.statement_uuid.canonical ==
          request.transaction_effect_evidence_uuid ||
      request.execution_attempt_uuid ==
          request.transaction_effect_evidence_uuid) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-IDENTITY-V1", 0,
                  "statement_execution_effect_uuid");
  }
  if (!request.cancellation_requested ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_output_columns == 0 ||
      request.maximum_output_cells == 0 ||
      request.maximum_output_columns > exec::kMaximumHeapOutputColumns) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT", 0,
                  "heap_result_resource_bounds");
  }

  const auto relational_validation = ValidateTypedRelationalDag(relational);
  if (!relational_validation.accepted) {
    const auto& issue = relational_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.node_id, issue.field_id);
  }
  const auto physical_validation = exec::ValidateTypedPhysicalNodeDag(physical);
  if (!physical_validation.accepted) {
    const auto& issue = physical_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.physical_node_id, issue.field_id);
  }
  const auto current_mga =
      exec::ValidateCurrentHeapPhysicalMgaAuthority(context, physical);
  if (!current_mga.ok) {
    return refuse(current_mga.diagnostic_code, 0, current_mga.detail);
  }
  auto mga_authority =
      exec::BuildCurrentHeapExecutionMgaAuthority(context, physical);
  const auto carrier_validation =
      exec::RevalidateCanonicalExecutionMgaAuthority(mga_authority, physical);
  if (!carrier_validation.ok) {
    return refuse(carrier_validation.diagnostic_code, 0,
                  carrier_validation.detail);
  }
  const std::string_view expected_relational_semantic =
      request.table_sample_profile.has_value()
          ? exec::HeapTableSampleSemanticId(*request.table_sample_profile)
          : std::string_view{"relation.source.v1"};
  const std::string_view expected_physical_implementation =
      request.table_sample_profile.has_value()
          ? exec::HeapTableSampleImplementationId(
                *request.table_sample_profile)
          : std::string_view{"scan.heap.v1"};
  if (relational.wire_version != 2 || relational.nodes.size() != 1 ||
      relational.root_node_id != relational.nodes.front().node_id ||
      relational.nodes.front().node_kind != RelationalDagNodeKind::kScan ||
      relational.nodes.front().semantic_variant_id !=
          expected_relational_semantic ||
      !relational.nodes.front().input_node_ids.empty() ||
      relational.nodes.front().required_object_uuids.size() != 1 ||
      relational.nodes.front().bound_expression_ids.empty() ||
      relational.nodes.front().bound_expression_ids.size() !=
          relational.nodes.front().output_descriptor_ids.size() ||
      relational.nodes.front().output_descriptor_ids.size() !=
          relational.expressions.size() ||
      relational.expressions.size() != relational.descriptors.size() ||
      relational.descriptors.size() != relational.outputs.size() ||
      relational.outputs.size() > request.maximum_output_columns ||
      relational.outputs.size() > exec::kMaximumHeapOutputColumns) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-PROFILE-V1", 0,
                  "bound_relation_source_one_leaf_complete_width");
  }
  if (physical.abi_version != 2 || physical.nodes.size() != 1 ||
      physical.root_physical_node_id != physical.nodes.front().physical_node_id ||
      physical.nodes.front().node_kind != exec::PhysicalNodeKind::kScan ||
      physical.nodes.front().implementation_id !=
          expected_physical_implementation ||
      !physical.nodes.front().input_physical_node_ids.empty() ||
      physical.nodes.front().relational_node_id != relational.root_node_id ||
      physical.nodes.front().output_descriptor_ids !=
          relational.nodes.front().output_descriptor_ids) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-PROFILE-V1", 0,
                  "scan.heap.v1_one_leaf_root");
  }

  const auto& relational_node = relational.nodes.front();
  const auto& physical_node = physical.nodes.front();
  if (!exec::IsCanonicalHeapBindingUuid(
          relational_node.required_object_uuids.front())) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                  physical_node.physical_node_id,
                  "relation_uuid");
  }

  std::vector<const RelationalOutputRecord*> ordered_outputs;
  ordered_outputs.reserve(relational.outputs.size());
  for (const auto& output : relational.outputs) {
    if (output.relation_node_id == relational_node.node_id) {
      ordered_outputs.push_back(&output);
    }
  }
  if (ordered_outputs.size() != relational.outputs.size()) {
    return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                  physical_node.physical_node_id,
                  "relation_output_coverage");
  }
  std::vector<const RelationalTypeDescriptor*> ordered_descriptors;
  ordered_descriptors.reserve(ordered_outputs.size());
  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> expression_ids;
  std::unordered_set<std::uint32_t> descriptor_ids;
  std::unordered_set<std::string> bound_column_uuids;
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    const auto expression = relational.expressions.begin() + ordinal;
    const auto descriptor = relational.descriptors.begin() + ordinal;
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        !output_ids.insert(output.output_id).second ||
        output.output_name_utf8.empty() ||
        output.descriptor_id != relational_node.output_descriptor_ids[ordinal] ||
        expression->expression_id != output.expression_id ||
        !expression_ids.insert(expression->expression_id).second ||
        relational_node.bound_expression_ids[ordinal] !=
            expression->expression_id ||
        expression->expression_kind != RelationalExpressionKind::kIdentifier ||
        !expression->child_expression_ids.empty() ||
        expression->result_descriptor_id != output.descriptor_id ||
        !expression->bound_name_uuid.has_value() ||
        !exec::IsCanonicalHeapBindingUuid(*expression->bound_name_uuid) ||
        !bound_column_uuids.insert(*expression->bound_name_uuid).second ||
        descriptor->descriptor_id != output.descriptor_id ||
        !descriptor_ids.insert(descriptor->descriptor_id).second ||
        !exec::IsCanonicalHeapBindingUuid(descriptor->descriptor_uuid) ||
        !exec::IsCanonicalHeapBindingUuid(descriptor->type_uuid) ||
        descriptor->nullability == RelationalNullability::kUnknown ||
        (descriptor->collation_uuid.has_value() &&
         !exec::IsCanonicalHeapBindingUuid(*descriptor->collation_uuid)) ||
        (descriptor->timezone_profile_id.has_value() &&
         descriptor->timezone_profile_id->empty())) {
      return refuse("QOW-DIAG-QRY-004-HEAP-RESULT-BINDING-V1",
                    physical_node.physical_node_id,
                    "relational_expression_descriptor_physical_agreement");
    }
    ordered_descriptors.push_back(&*descriptor);
  }

  exec::CanonicalHeapPhysicalDagDispatchRequest registration_request;
  registration_request.context = &request.context;
  registration_request.relational_dag = &request.relational_dag;
  registration_request.physical_dag = request.selected_physical_dag;
  registration_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  registration_request.maximum_decoded_bytes = request.maximum_decoded_bytes;
  registration_request.maximum_output_rows = request.maximum_output_rows;
  registration_request.maximum_output_columns = request.maximum_output_columns;
  registration_request.maximum_output_cells = request.maximum_output_cells;
  registration_request.cancellation_requested = request.cancellation_requested;
  registration_request.mga_authority = mga_authority;
  registration_request.heap_read_authority_cohort = request.authority_cohort;
  registration_request.table_sample_profile = request.table_sample_profile;
  auto built = exec::BuildHeapPhysicalRegistration(registration_request);
  if (!built.diagnostic.ok || !built.registration.has_value() ||
      built.observation == nullptr) {
    if (built.diagnostic.ok) {
      built.diagnostic = exec::HeapAcquisitionRefusal(
          "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
          "engine heap executor registration is unavailable");
    }
    return refuse(std::move(built.diagnostic.diagnostic_code),
                  physical_node.physical_node_id,
                  std::move(built.diagnostic.detail));
  }

  CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = request.selected_physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      request.selected_physical_dag.statistics_snapshot_uuid;
  selected.mga_authority = mga_authority;
  selected.available_executors.push_back(std::move(*built.registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      context.statement_uuid.canonical;
  selected.result_publication_request.execution_attempt_uuid =
      request.execution_attempt_uuid;
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.column_bindings.reserve(
      ordered_outputs.size());
  for (std::size_t ordinal = 0; ordinal < ordered_outputs.size(); ++ordinal) {
    const auto& output = *ordered_outputs[ordinal];
    const auto& descriptor = *ordered_descriptors[ordinal];
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = ordinal;
    published.name_utf8 = output.output_name_utf8;
    published.descriptor_uuid = descriptor.descriptor_uuid;
    published.type_uuid = descriptor.type_uuid;
    published.nullability =
        descriptor.nullability == RelationalNullability::kNullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull;
    published.collation_uuid = descriptor.collation_uuid;
    published.timezone_profile_id = descriptor.timezone_profile_id;
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  selected.result_publication_request.transaction_effect_evidence_uuid =
      request.transaction_effect_evidence_uuid;
  selected.result_publication_request.maximum_row_count =
      request.maximum_output_rows;
  return ExecuteCanonicalOptimizerSelectedDag(selected);
}

}  // namespace scratchbird::engine::internal_api
