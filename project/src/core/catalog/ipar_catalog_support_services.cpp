// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_catalog_support_services.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::catalog {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::string EpochKey(const IparSystemMetadataRequest& request) {
  std::ostringstream out;
  out << "session=" << request.session_uuid
      << "|catalog=" << request.catalog_epoch
      << "|security=" << request.security_epoch
      << "|policy=" << request.policy_epoch
      << "|language=" << request.language_epoch << "|views=";
  auto views = request.requested_sys_views;
  std::sort(views.begin(), views.end());
  for (const auto& view : views) {
    out << view << ';';
  }
  return out.str();
}

}  // namespace

bool IparCatalogAuthorityBoundarySafe(
    const IparCatalogAuthorityBoundary& authority) {
  return authority.durable_transaction_inventory_authority &&
         !authority.support_service_finality_authority &&
         !authority.parser_finality_authority &&
         !authority.client_finality_authority &&
         !authority.provider_finality_authority;
}

IparOnlineDdlPlan PlanIparOnlineDdl(const IparOnlineDdlRequest& request) {
  IparOnlineDdlPlan plan;
  Add(&plan.evidence, "IPAR-P2-06");
  Add(&plan.evidence, "ddl_finality_authority=durable_mga_transaction_inventory");
  if (!IparCatalogAuthorityBoundarySafe(request.authority)) {
    plan.diagnostic_code = "IPAR_DDL_AUTHORITY_UNSAFE";
    return plan;
  }
  if (request.job_uuid.empty() || request.object_uuid.empty() ||
      request.catalog_epoch == 0 || request.security_epoch == 0 ||
      !request.committed_metadata_snapshot_present) {
    plan.diagnostic_code = "IPAR_DDL_REQUEST_INCOMPLETE";
    return plan;
  }
  plan.recovery_classified = request.recovery_replay;
  if (request.cancellation_requested) {
    plan.accepted = true;
    plan.rollback_required = true;
    plan.diagnostic_code = "IPAR_DDL_CANCEL_ROLLBACK";
    plan.progress_states = {"admitted", "cancelling", "rolling_back"};
    Add(&plan.evidence, "ddl_partial_publish=false");
    return plan;
  }
  if (request.dependency_conflict) {
    plan.diagnostic_code = "IPAR_DDL_DEPENDENCY_CONFLICT";
    Add(&plan.evidence, "ddl_serializes_true_dependency_conflict=true");
    return plan;
  }
  plan.accepted = true;
  plan.online = request.online_requested && request.estimated_rows > 0;
  plan.final_publish_uses_short_exclusive_lock = true;
  plan.diagnostic_code = "IPAR_DDL_ONLINE_PLAN_READY";
  plan.lock_partitions.push_back("catalog:" + request.object_uuid);
  for (const auto& dependency : request.dependency_uuids) {
    plan.lock_partitions.push_back("dependency:" + dependency);
  }
  plan.progress_states = {"admitted", "prebuild", "validate", "publish_ready",
                          "short_publish", "complete"};
  Add(&plan.evidence, plan.online ? "ddl_online=true" : "ddl_online=false");
  Add(&plan.evidence, "ddl_unrelated_dml_frozen=false");
  return plan;
}

IparSystemMetadataMaterialization IparSystemMetadataCache::GetOrMaterialize(
    const IparSystemMetadataRequest& request) {
  IparSystemMetadataMaterialization result;
  result.cache_key = EpochKey(request);
  Add(&result.evidence, "IPAR-P2-07");
  if (request.session_uuid.empty() || request.catalog_epoch == 0 ||
      request.security_epoch == 0 || request.policy_epoch == 0 ||
      request.language_epoch == 0 || request.requested_sys_views.empty()) {
    result.diagnostic_code = "IPAR_METADATA_EPOCH_INCOMPLETE";
    return result;
  }
  const auto found = entries_.find(result.cache_key);
  if (found != entries_.end()) {
    result = found->second;
    result.cache_hit = true;
    Add(&result.evidence, "metadata_cache_hit=true");
    return result;
  }
  result.accepted = true;
  result.diagnostic_code = "IPAR_METADATA_MATERIALIZED";
  result.materialized_views = request.requested_sys_views;
  std::sort(result.materialized_views.begin(), result.materialized_views.end());
  result.materialized_views.erase(
      std::unique(result.materialized_views.begin(), result.materialized_views.end()),
      result.materialized_views.end());
  result.materialized_object_count = request.authorized_object_uuids.size();
  Add(&result.evidence, "metadata_authorized_tree=true");
  Add(&result.evidence, "metadata_epoch_keyed=true");
  entries_[result.cache_key] = result;
  return result;
}

void IparSystemMetadataCache::InvalidateCatalogEpoch(u64 catalog_epoch) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.find("|catalog=" + std::to_string(catalog_epoch)) !=
        std::string::npos) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

IparConstraintBackfillPlan PlanIparConstraintBackfill(
    const IparConstraintBackfillRequest& request) {
  IparConstraintBackfillPlan plan;
  Add(&plan.evidence, "IPAR-P2-11");
  Add(&plan.evidence, "constraint_publish_finality=durable_mga_transaction_inventory");
  if (!IparCatalogAuthorityBoundarySafe(request.authority)) {
    plan.diagnostic_code = "IPAR_CONSTRAINT_BACKFILL_AUTHORITY_UNSAFE";
    return plan;
  }
  if (request.job_uuid.empty() || request.table_uuid.empty() ||
      request.constraint_uuid.empty() || request.supporting_index_uuid.empty() ||
      request.catalog_epoch == 0 ||
      !request.committed_snapshot_present) {
    plan.diagnostic_code = "IPAR_CONSTRAINT_BACKFILL_REQUEST_INCOMPLETE";
    return plan;
  }
  plan.remaining_rows = request.source_row_count > request.validated_row_count
                            ? request.source_row_count - request.validated_row_count
                            : 0;
  if (request.cancellation_requested || request.violation_detected) {
    plan.accepted = true;
    plan.rollback_required = true;
    plan.partial_visible_state = false;
    plan.diagnostic_code = request.violation_detected
                               ? "IPAR_CONSTRAINT_VALIDATION_VIOLATION"
                               : "IPAR_CONSTRAINT_BACKFILL_CANCELLED";
    plan.progress_states = {"admitted", "validating", "rolling_back"};
    return plan;
  }
  plan.accepted = true;
  plan.ready_to_publish = request.final_publish_requested && plan.remaining_rows == 0;
  plan.partial_visible_state = false;
  plan.diagnostic_code = plan.ready_to_publish
                             ? "IPAR_CONSTRAINT_READY_FOR_SHORT_PUBLISH"
                             : "IPAR_CONSTRAINT_BACKFILL_IN_PROGRESS";
  plan.progress_states = {"admitted", "backfill", "validate",
                          plan.ready_to_publish ? "publish_ready" : "running"};
  Add(&plan.evidence, "constraint_partial_visible_state=false");
  return plan;
}

bool IparRuntimeRoutineCacheKey::operator<(
    const IparRuntimeRoutineCacheKey& other) const {
  if (object_uuid != other.object_uuid) return object_uuid < other.object_uuid;
  if (sblr_digest != other.sblr_digest) return sblr_digest < other.sblr_digest;
  if (catalog_epoch != other.catalog_epoch) return catalog_epoch < other.catalog_epoch;
  if (security_epoch != other.security_epoch) return security_epoch < other.security_epoch;
  return dependency_epoch < other.dependency_epoch;
}

IparRuntimeRoutineLookup IparRuntimeRoutineCache::Put(IparRuntimeRoutinePlan plan) {
  IparRuntimeRoutineLookup result;
  Add(&result.evidence, "IPAR-P4-09");
  if (plan.key.object_uuid.empty() || plan.key.sblr_digest.empty() ||
      plan.key.catalog_epoch == 0 || plan.key.security_epoch == 0 ||
      plan.key.dependency_epoch == 0 || plan.compiled_handle.empty() ||
      !plan.engine_sblr_only || plan.parser_execution_authority ||
      !(plan.trigger || plan.constraint || plan.routine)) {
    result.diagnostic_code = "IPAR_RUNTIME_ROUTINE_CACHE_UNSAFE";
    return result;
  }
  entries_[plan.key] = plan;
  result.accepted = true;
  result.plan = std::move(plan);
  result.diagnostic_code = "IPAR_RUNTIME_ROUTINE_CACHE_STORED";
  Add(&result.evidence, "runtime_cache_engine_sblr_only=true");
  return result;
}

IparRuntimeRoutineLookup IparRuntimeRoutineCache::Lookup(
    const IparRuntimeRoutineCacheKey& key) const {
  IparRuntimeRoutineLookup result;
  Add(&result.evidence, "IPAR-P4-09");
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    result.diagnostic_code = "IPAR_RUNTIME_ROUTINE_CACHE_MISS";
    return result;
  }
  result.hit = true;
  result.accepted = true;
  result.plan = found->second;
  result.diagnostic_code = "IPAR_RUNTIME_ROUTINE_CACHE_HIT";
  Add(&result.evidence, "runtime_cache_dependency_epoch_matched=true");
  return result;
}

u64 IparRuntimeRoutineCache::InvalidateDependencyEpoch(u64 dependency_epoch) {
  u64 erased = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.dependency_epoch <= dependency_epoch) {
      it = entries_.erase(it);
      ++erased;
    } else {
      ++it;
    }
  }
  return erased;
}

}  // namespace scratchbird::core::catalog
