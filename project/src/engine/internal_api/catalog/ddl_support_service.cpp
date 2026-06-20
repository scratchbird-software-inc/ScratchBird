// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/ddl_support_service.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/pinned_descriptor_cache.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string UInt64Hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << value;
  return out.str();
}

std::string StableDigest(const std::vector<std::string>& parts) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& part : parts) {
    for (const unsigned char ch : part) {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return "fnv1a64:" + UInt64Hex(hash);
}

std::vector<std::string> SortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

std::string ObjectUuidFromRequest(const EngineApiRequest& request) {
  if (!request.target_object.uuid.canonical.empty()) {
    return request.target_object.uuid.canonical;
  }
  if (!request.bound_object_identity.object_uuid.canonical.empty()) {
    return request.bound_object_identity.object_uuid.canonical;
  }
  return {};
}

std::string ObjectKindFromState(const EngineCatalogObjectLifecycleState& state,
                                const std::string& object_uuid,
                                const std::string& fallback = {}) {
  for (const auto& object : state.objects) {
    if (object.object_uuid == object_uuid) {
      return object.object_kind;
    }
  }
  return fallback;
}

std::vector<CatalogDdlDependencyEdge> DependencyEdges(
    const EngineCatalogObjectLifecycleState& state) {
  std::vector<CatalogDdlDependencyEdge> edges;
  for (const auto& dependency : state.dependencies) {
    if (dependency.source_uuid.empty() || dependency.dependency_uuid.empty()) {
      continue;
    }
    edges.push_back({dependency.source_uuid,
                     dependency.source_kind,
                     dependency.dependency_uuid,
                     dependency.dependency_kind,
                     "invalidate_dependent"});
  }
  for (const auto& object : state.objects) {
    if (object.object_uuid.empty() || object.schema_uuid.empty()) {
      continue;
    }
    edges.push_back({object.object_uuid,
                     object.object_kind,
                     object.schema_uuid,
                     "ownership_parent",
                     "invalidate_child"});
  }
  for (const auto& dependency : state.constraint_dependencies) {
    if (dependency.constraint_uuid.empty() ||
        dependency.dependency_object_uuid.empty()) {
      continue;
    }
    edges.push_back({dependency.constraint_uuid,
                     "constraint",
                     dependency.dependency_object_uuid,
                     "constraint:" + dependency.dependency_kind,
                     dependency.invalidation_action.empty()
                         ? "revalidate_required"
                         : dependency.invalidation_action});
  }
  for (const auto& support : state.constraint_support_structures) {
    if (support.constraint_uuid.empty() || support.support_uuid.empty()) {
      continue;
    }
    edges.push_back({support.constraint_uuid,
                     "constraint",
                     support.support_uuid,
                     "constraint:support_structure",
                     "invalidate_support_binding"});
  }
  std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.dependency_uuid,
                    lhs.source_uuid,
                    lhs.dependency_kind,
                    lhs.invalidation_action) <
           std::tie(rhs.dependency_uuid,
                    rhs.source_uuid,
                    rhs.dependency_kind,
                    rhs.invalidation_action);
  });
  edges.erase(std::unique(edges.begin(),
                          edges.end(),
                          [](const auto& lhs, const auto& rhs) {
                            return lhs.source_uuid == rhs.source_uuid &&
                                   lhs.source_kind == rhs.source_kind &&
                                   lhs.dependency_uuid == rhs.dependency_uuid &&
                                   lhs.dependency_kind == rhs.dependency_kind &&
                                   lhs.invalidation_action == rhs.invalidation_action;
                          }),
              edges.end());
  return edges;
}

std::string ClosureCacheKey(const EngineCatalogObjectLifecycleState& state,
                            const EngineRequestContext& context,
                            const std::string& root_uuid,
                            const std::vector<CatalogDdlDependencyEdge>& edges) {
  std::vector<std::string> parts;
  parts.push_back("root=" + root_uuid);
  parts.push_back("catalog_epoch=" + std::to_string(state.metadata_epoch));
  parts.push_back("security_epoch=" + std::to_string(context.security_epoch));
  parts.push_back("policy_epoch=" + std::to_string(context.resource_epoch));
  for (const auto& edge : edges) {
    parts.push_back(edge.source_uuid + ">" + edge.dependency_uuid + ":" +
                    edge.dependency_kind + ":" + edge.invalidation_action);
  }
  return StableDigest(parts);
}

CatalogDdlDependencyClosure BuildClosure(
    const EngineCatalogObjectLifecycleState& state,
    const EngineRequestContext& context,
    const std::string& root_uuid,
    std::vector<CatalogDdlDependencyEdge> edges) {
  CatalogDdlDependencyClosure closure;
  closure.root_uuid = root_uuid;
  closure.catalog_epoch = state.metadata_epoch;
  closure.security_epoch = context.security_epoch;
  closure.policy_epoch = context.resource_epoch;
  closure.cache_key = ClosureCacheKey(state, context, root_uuid, edges);

  std::map<std::string, std::vector<CatalogDdlDependencyEdge>> by_dependency;
  for (const auto& edge : edges) {
    by_dependency[edge.dependency_uuid].push_back(edge);
  }

  std::set<std::string> seen;
  std::vector<std::string> queue;
  if (!root_uuid.empty()) {
    seen.insert(root_uuid);
    queue.push_back(root_uuid);
    closure.affected_object_uuids.push_back(root_uuid);
  }

  for (std::size_t index = 0; index < queue.size(); ++index) {
    const auto found = by_dependency.find(queue[index]);
    if (found == by_dependency.end()) {
      continue;
    }
    for (const auto& edge : found->second) {
      closure.edges.push_back(edge);
      if (!edge.source_uuid.empty() && seen.insert(edge.source_uuid).second) {
        queue.push_back(edge.source_uuid);
        closure.affected_object_uuids.push_back(edge.source_uuid);
      }
    }
  }

  closure.affected_object_uuids = SortedUnique(closure.affected_object_uuids);
  std::vector<std::string> digest_parts = closure.affected_object_uuids;
  for (const auto& edge : closure.edges) {
    digest_parts.push_back(edge.source_uuid + ">" + edge.dependency_uuid + ":" +
                           edge.invalidation_action);
  }
  closure.closure_digest = StableDigest(digest_parts);
  return closure;
}

EngineTypedValue TextValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddSupportRow(EngineApiResult* result,
                   std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = GenerateCrudEngineUuid("row");
  for (auto& field : fields) {
    row.fields.push_back({std::move(field.first), TextValue(std::move(field.second))});
  }
  result->result_shape.result_kind = "catalog_ddl_support_service";
  result->result_shape.rows.push_back(std::move(row));
}

std::string StageDescriptorPayload(const EngineCatalogDdlSupportRequest& request,
                                   const EngineObjectReference& object) {
  std::vector<std::string> parts;
  parts.push_back("object_uuid=" + object.uuid.canonical);
  parts.push_back("object_kind=" + object.object_kind);
  parts.push_back("validation=complete");
  parts.push_back("construction_phase=prebuild");
  parts.push_back("final_publish_lock_held=false");
  parts.push_back("parser_sql_authority=false");
  if (object.object_kind == "table") {
    parts.push_back("column_count=" + std::to_string(request.columns.size()));
  } else if (object.object_kind == "index") {
    parts.push_back("index_definition_count=" + std::to_string(request.indexes.size()));
  } else if (object.object_kind == "constraint") {
    parts.push_back("constraint_definition_count=" + std::to_string(request.constraints.size()));
  }
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) out.push_back(';');
    out += part;
  }
  return out;
}

std::vector<CatalogDdlStagedDescriptor> BuildStagedDescriptors(
    const EngineCatalogDdlSupportRequest& request) {
  std::vector<EngineObjectReference> objects = request.stage_objects;
  if (objects.empty() && !request.target_object.uuid.canonical.empty()) {
    objects.push_back(request.target_object);
  }
  std::vector<CatalogDdlStagedDescriptor> staged;
  staged.reserve(objects.size());
  for (auto object : objects) {
    if (object.uuid.canonical.empty() || object.object_kind.empty()) {
      continue;
    }
    CatalogDdlStagedDescriptor descriptor;
    descriptor.object = object;
    descriptor.descriptor.descriptor_uuid = object.uuid;
    descriptor.descriptor.descriptor_kind = "sys.catalog.staged." + object.object_kind;
    descriptor.descriptor.canonical_type_name = object.object_kind;
    descriptor.descriptor.encoded_descriptor = StageDescriptorPayload(request, object);
    descriptor.construction_phase = "validate_and_prebuild";
    descriptor.validation_state = "validated";
    descriptor.built_before_final_publish_lock = true;
    descriptor.final_publish_lock_held = false;
    descriptor.parser_sql_authority = false;
    staged.push_back(std::move(descriptor));
  }
  return staged;
}

std::vector<CatalogDdlCacheInvalidation> PredictCacheInvalidations(
    const EngineCatalogObjectLifecycleState& state,
    const CatalogDdlDependencyClosure& closure) {
  std::vector<CatalogDdlCacheInvalidation> invalidations;
  for (const auto& object_uuid : closure.affected_object_uuids) {
    const std::string kind = ObjectKindFromState(state, object_uuid, "object");
    invalidations.push_back({"descriptor_cache", object_uuid, "catalog_dependency_closure"});
    invalidations.push_back({"name_uuid_cache", object_uuid, "catalog_epoch_or_name_mapping"});
    invalidations.push_back({"prepared_context", object_uuid, "descriptor_epoch_change"});
    if (kind == "table" || kind == "view" || kind == "index" ||
        kind == "constraint" || kind == "policy") {
      invalidations.push_back({"plan_cache", object_uuid, "relational_metadata_change"});
    }
    if (kind == "procedure" || kind == "function" || kind == "trigger" ||
        kind == "package" || kind == "view") {
      invalidations.push_back({"compiled_routine_cache", object_uuid, "routine_dependency_change"});
    }
  }
  std::sort(invalidations.begin(), invalidations.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.cache_family, lhs.object_uuid, lhs.reason) <
           std::tie(rhs.cache_family, rhs.object_uuid, rhs.reason);
  });
  invalidations.erase(std::unique(invalidations.begin(),
                                  invalidations.end(),
                                  [](const auto& lhs, const auto& rhs) {
                                    return lhs.cache_family == rhs.cache_family &&
                                           lhs.object_uuid == rhs.object_uuid &&
                                           lhs.reason == rhs.reason;
                                  }),
                      invalidations.end());
  return invalidations;
}

std::vector<CatalogDdlPreparedContextInvalidation> PredictPreparedInvalidations(
    const EngineCatalogDdlSupportRequest& request,
    const CatalogDdlDependencyClosure& closure) {
  std::set<std::string> affected(closure.affected_object_uuids.begin(),
                                 closure.affected_object_uuids.end());
  std::vector<CatalogDdlPreparedContextInvalidation> invalidations;
  for (const auto& prepared : request.prepared_contexts) {
    CatalogDdlPreparedContextInvalidation invalidation;
    invalidation.prepared_context_uuid = prepared.prepared_context_uuid;
    if (prepared.catalog_epoch != 0 &&
        prepared.catalog_epoch < closure.catalog_epoch + 1) {
      invalidation.reason = "catalog_epoch_stale";
    }
    if (prepared.security_epoch != 0 &&
        prepared.security_epoch != request.context.security_epoch) {
      invalidation.reason = "security_epoch_stale";
    }
    if (prepared.policy_epoch != 0 &&
        prepared.policy_epoch != request.context.resource_epoch) {
      invalidation.reason = "policy_epoch_stale";
    }
    for (const auto& dependency : prepared.dependent_object_uuids) {
      if (affected.count(dependency) != 0) {
        invalidation.matched_object_uuids.push_back(dependency);
      }
    }
    invalidation.matched_object_uuids = SortedUnique(invalidation.matched_object_uuids);
    if (!invalidation.matched_object_uuids.empty()) {
      invalidation.reason = "catalog_dependency_closure";
    }
    if (!invalidation.reason.empty()) {
      invalidations.push_back(std::move(invalidation));
    }
  }
  return invalidations;
}

CatalogDdlPublishPlan BuildPublishPlan(const CatalogDdlDependencyClosure& closure) {
  CatalogDdlPublishPlan plan;
  plan.target_catalog_epoch = closure.catalog_epoch + 1;
  plan.publish_steps = {"validate",
                        "prebuild_descriptors",
                        "prebuild_backfill_plan",
                        "reserve_uuid_result",
                        "atomic_catalog_epoch_publish",
                        "exact_dependency_invalidation",
                        "rcu_snapshot_retire"};
  plan.final_publish_lock_scope = "catalog_epoch_publish_only";
  plan.rollback_recovery_authority = "durable_transaction_inventory";
  plan.validation_before_publish = true;
  plan.prebuild_before_final_lock = true;
  plan.final_publish_short_section = true;
  plan.partial_state_visible = false;
  plan.uuid_returning_result = true;
  return plan;
}

CatalogDdlImmutableSnapshot SnapshotFromClosure(
    const CatalogDdlDependencyClosure& closure) {
  CatalogDdlImmutableSnapshot snapshot;
  snapshot.catalog_epoch = closure.catalog_epoch;
  snapshot.object_uuids = closure.affected_object_uuids;
  snapshot.snapshot_digest = closure.closure_digest;
  snapshot.immutable = true;
  snapshot.finality_authority_cached = false;
  return snapshot;
}

}  // namespace

CatalogDdlDependencyClosure CatalogDdlDependencyClosureCache::LookupOrBuild(
    const EngineCatalogObjectLifecycleState& state,
    const EngineRequestContext& context,
    const std::string& root_uuid) {
  const auto edges = DependencyEdges(state);
  const std::string key = ClosureCacheKey(state, context, root_uuid, edges);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto closure : closures_) {
      if (closure.cache_key == key) {
        closure.cache_hit = true;
        ++stats_.hits;
        return closure;
      }
    }
    ++stats_.misses;
  }

  auto closure = BuildClosure(state, context, root_uuid, edges);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closures_.push_back(closure);
    ++stats_.builds;
  }
  return closure;
}

void CatalogDdlDependencyClosureCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  closures_.clear();
  ++stats_.clears;
}

CatalogDdlDependencyClosureCacheStats CatalogDdlDependencyClosureCache::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

CatalogDdlDependencyClosureCache& GlobalCatalogDdlDependencyClosureCache() {
  static CatalogDdlDependencyClosureCache cache;
  return cache;
}

CatalogDdlImmutableSnapshot CatalogDdlImmutableSnapshotPublisher::Publish(
    CatalogDdlImmutableSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot.generation = next_generation_++;
  auto stored = std::make_shared<const CatalogDdlImmutableSnapshot>(snapshot);
  snapshots_.push_back(std::move(stored));
  active_readers_.push_back({snapshot.generation, 0});
  return snapshot;
}

CatalogDdlSnapshotReadHandle CatalogDdlImmutableSnapshotPublisher::AcquireLatest() {
  std::lock_guard<std::mutex> lock(mutex_);
  CatalogDdlSnapshotReadHandle handle;
  if (snapshots_.empty()) {
    return handle;
  }
  handle.snapshot = snapshots_.back();
  handle.generation = handle.snapshot->generation;
  for (auto& readers : active_readers_) {
    if (readers.first == handle.generation) {
      ++readers.second;
      break;
    }
  }
  return handle;
}

void CatalogDdlImmutableSnapshotPublisher::Release(
    const CatalogDdlSnapshotReadHandle& handle) {
  if (handle.generation == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& readers : active_readers_) {
    if (readers.first == handle.generation) {
      if (readers.second != 0) {
        --readers.second;
      }
      return;
    }
  }
}

std::uint64_t CatalogDdlImmutableSnapshotPublisher::RetireUpTo(
    std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::uint64_t retired = 0;
  const std::uint64_t latest = snapshots_.empty() ? 0 : snapshots_.back()->generation;
  for (auto it = snapshots_.begin(); it != snapshots_.end();) {
    const std::uint64_t candidate = (*it)->generation;
    if (candidate > generation || candidate == latest) {
      ++it;
      continue;
    }
    std::uint64_t readers = 0;
    for (const auto& entry : active_readers_) {
      if (entry.first == candidate) {
        readers = entry.second;
        break;
      }
    }
    if (readers != 0) {
      ++it;
      continue;
    }
    it = snapshots_.erase(it);
    active_readers_.erase(std::remove_if(active_readers_.begin(),
                                         active_readers_.end(),
                                         [candidate](const auto& entry) {
                                           return entry.first == candidate;
                                         }),
                          active_readers_.end());
    ++retired;
  }
  return retired;
}

std::uint64_t CatalogDdlImmutableSnapshotPublisher::ActiveReaders(
    std::uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& readers : active_readers_) {
    if (readers.first == generation) {
      return readers.second;
    }
  }
  return 0;
}

std::uint64_t CatalogDdlImmutableSnapshotPublisher::RetainedSnapshotCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshots_.size();
}

void CatalogDdlImmutableSnapshotPublisher::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  next_generation_ = 1;
  snapshots_.clear();
  active_readers_.clear();
}

CatalogDdlImmutableSnapshotPublisher& GlobalCatalogDdlImmutableSnapshotPublisher() {
  static CatalogDdlImmutableSnapshotPublisher publisher;
  return publisher;
}

EngineCatalogDdlSupportResult EngineCatalogDdlSupportService(
    const EngineCatalogDdlSupportRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? "catalog.ddl_support" : request.operation_id;
  if (request.context.database_path.empty()) {
    return MakeCrudDiagnosticResult<EngineCatalogDdlSupportResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "database_path_required"));
  }
  const std::string root_uuid = ObjectUuidFromRequest(request);
  if (root_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineCatalogDdlSupportResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "target_object_uuid_required"));
  }

  const auto loaded = LoadCatalogObjectLifecycleState(request.context);
  if (!loaded.ok) {
    return MakeCrudDiagnosticResult<EngineCatalogDdlSupportResult>(
        request.context, operation_id, loaded.diagnostic);
  }

  auto result =
      MakeCrudSuccessResult<EngineCatalogDdlSupportResult>(request.context, operation_id);
  result.primary_object = request.target_object;
  if (result.primary_object.object_kind.empty()) {
    result.primary_object.object_kind =
        ObjectKindFromState(loaded.state, root_uuid, "catalog_object");
  }
  result.primary_object.uuid.canonical = root_uuid;
  result.dependency_closure =
      GlobalCatalogDdlDependencyClosureCache().LookupOrBuild(loaded.state,
                                                             request.context,
                                                             root_uuid);
  result.publish_plan = BuildPublishPlan(result.dependency_closure);
  result.staged_descriptors = BuildStagedDescriptors(request);
  result.cache_invalidations =
      PredictCacheInvalidations(loaded.state, result.dependency_closure);
  result.prepared_context_invalidations =
      PredictPreparedInvalidations(request, result.dependency_closure);

  if (request.apply_descriptor_cache_invalidation) {
    std::set<std::string> invalidated_keys;
    for (const auto& object_uuid : result.dependency_closure.affected_object_uuids) {
      auto event = CatalogPinnedDescriptorInvalidationEventForMutation(
          request.mutation_source.empty() ? "catalog_object_alter"
                                          : request.mutation_source,
          object_uuid,
          result.publish_plan.target_catalog_epoch);
      event.reason = "catalog_dependency_closure";
      const auto invalidated = GlobalCatalogPinnedDescriptorCache().Invalidate(event);
      for (const auto& entry : invalidated.invalidated_entries) {
        invalidated_keys.insert(entry.cache_key);
      }
    }
    result.descriptor_cache_invalidated_entries = invalidated_keys.size();
  }

  if (request.publish_immutable_snapshot) {
    const auto snapshot =
        GlobalCatalogDdlImmutableSnapshotPublisher().Publish(
            SnapshotFromClosure(result.dependency_closure));
    result.immutable_snapshot_generation = snapshot.generation;
  }

  AddApiBehaviorEvidence(&result, "catalog_ddl_support_service", "active");
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_dependency_closure_digest",
                         result.dependency_closure.closure_digest);
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_dependency_closure_cache",
                         result.dependency_closure.cache_hit ? "hit" : "miss");
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_predicted_epoch",
                         std::to_string(result.publish_plan.target_catalog_epoch));
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_descriptor_cache_invalidated",
                         std::to_string(result.descriptor_cache_invalidated_entries));
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_prepared_context_invalidated",
                         std::to_string(result.prepared_context_invalidations.size()));
  AddApiBehaviorEvidence(&result, "catalog_ddl_parser_sql_authority", "false");
  AddApiBehaviorEvidence(&result,
                         "catalog_ddl_mga_finality_authority",
                         "durable_transaction_inventory");

  AddSupportRow(&result,
                {{"target_object_uuid", root_uuid},
                 {"affected_object_count",
                  std::to_string(result.dependency_closure.affected_object_uuids.size())},
                 {"staged_descriptor_count",
                  std::to_string(result.staged_descriptors.size())},
                 {"cache_invalidation_count",
                  std::to_string(result.cache_invalidations.size())},
                 {"prepared_context_invalidation_count",
                  std::to_string(result.prepared_context_invalidations.size())},
                 {"final_publish_lock_scope", result.publish_plan.final_publish_lock_scope},
                 {"final_publish_short_section",
                  result.publish_plan.final_publish_short_section ? "true" : "false"},
                 {"partial_state_visible",
                  result.publish_plan.partial_state_visible ? "true" : "false"},
                 {"rollback_recovery_authority",
                  result.publish_plan.rollback_recovery_authority},
                 {"parser_sql_authority", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
