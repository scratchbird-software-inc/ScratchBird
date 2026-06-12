// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lock_wait_lifecycle.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status ErrorStatus(Severity severity = Severity::error) {
  return {StatusCode::platform_required_feature_missing, severity, Subsystem::transaction_mga};
}

bool IsNone(const std::string& value) {
  return value.empty() || value == "none";
}

bool LooksLikeUuidText(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (ch != '-') { return false; }
      continue;
    }
    const bool digit = ch >= '0' && ch <= '9';
    const bool lower_hex = ch >= 'a' && ch <= 'f';
    const bool upper_hex = ch >= 'A' && ch <= 'F';
    if (!digit && !lower_hex && !upper_hex) { return false; }
  }
  return true;
}

bool LooksLikeUuidV7Text(const std::string& value) {
  return LooksLikeUuidText(value) && value[14] == '7';
}

bool IsClusterScope(MGALockScopeKind scope_kind) {
  return scope_kind == MGALockScopeKind::cluster_transaction_identifier ||
         scope_kind == MGALockScopeKind::cluster_member_epoch ||
         scope_kind == MGALockScopeKind::cluster_routing_or_placement_unit;
}

bool IsClusterMode(MGALockMode mode) {
  return mode == MGALockMode::cluster_prepare || mode == MGALockMode::cluster_decision;
}

bool IsValidOwnerKind(MGALockOwnerKind owner_kind) {
  return owner_kind != MGALockOwnerKind::unknown;
}

bool IsValidResourceKind(MGAWaitResourceKind resource_kind) {
  return resource_kind != MGAWaitResourceKind::unknown;
}

bool IsValidScopeKind(MGALockScopeKind scope_kind) {
  return scope_kind != MGALockScopeKind::unknown;
}

bool IsValidMode(MGALockMode mode) {
  return mode != MGALockMode::unknown;
}

bool IsValidWaitPolicy(MGAWaitPolicy wait_policy) {
  return wait_policy != MGAWaitPolicy::unknown;
}

bool IsValidPriority(MGAPriorityClass priority_class) {
  return priority_class != MGAPriorityClass::unknown;
}

bool IsTransactionScopedWrite(const MGALockRequest& request) {
  return request.owner_kind == MGALockOwnerKind::transaction &&
         (request.mode == MGALockMode::exclusive_write ||
          request.mode == MGALockMode::update_intent ||
          request.mode == MGALockMode::range_update ||
          request.mode == MGALockMode::range_exclusive ||
          request.mode == MGALockMode::schema_modify);
}

std::string ResourceKey(const MGALockRequest& request) {
  std::string key = MGAWaitResourceKindName(request.resource_kind);
  key += "|";
  key += request.database_uuid;
  key += "|";
  key += MGALockScopeKindName(request.scope_kind);
  key += "|";
  if (!IsNone(request.record_uuid)) {
    key += "record:";
    key += request.record_uuid;
  } else if (!IsNone(request.scope_uuid)) {
    key += "scope:";
    key += request.scope_uuid;
  } else if (!IsNone(request.table_uuid)) {
    key += "table:";
    key += request.table_uuid;
  } else {
    key += "database";
  }
  return key;
}

bool SameOwner(const MGALockRequest& request, const MGALockGrant& grant) {
  return request.owner_uuid == grant.owner_uuid && request.owner_kind == grant.owner_kind;
}

bool SameResource(const MGALockRequest& request, const MGALockGrant& grant) {
  return request.database_uuid == grant.database_uuid &&
         request.scope_kind == grant.scope_kind &&
         request.scope_uuid == grant.scope_uuid;
}

u16 ModeIndex(MGALockMode mode) {
  return static_cast<u16>(mode);
}

bool DeadlineExpired(const MGALockRequest& request, u64 now_millis) {
  if (request.wait_policy == MGAWaitPolicy::no_wait) { return true; }
  if (request.wait_policy != MGAWaitPolicy::wait_timeout &&
      request.wait_policy != MGAWaitPolicy::reference_compatible_wait &&
      request.wait_policy != MGAWaitPolicy::maintenance_window_wait) {
    return false;
  }
  if (request.timeout_millis == 0) { return true; }
  return now_millis >= request.requested_at_millis &&
         now_millis - request.requested_at_millis >= request.timeout_millis;
}

u64 DeadlineMillis(const MGALockRequest& request) {
  if (request.timeout_millis == 0) { return 0; }
  return request.requested_at_millis + request.timeout_millis;
}

bool WaitDeadlineExpired(const MGALockWait& wait, u64 now_millis) {
  return wait.deadline_millis != 0 && now_millis >= wait.deadline_millis;
}

MGALockLifecycleResult Result(MGALockLifecycleDecision decision,
                              Status status,
                              DiagnosticRecord diagnostic = {}) {
  MGALockLifecycleResult result;
  result.status = status;
  result.decision = decision;
  result.diagnostic = std::move(diagnostic);
  return result;
}

MGALockLifecycleResult ErrorResult(MGALockLifecycleDecision decision,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   const MGALockRequest* request,
                                   const MGALockWait* wait,
                                   std::string required_action,
                                   Severity severity = Severity::error) {
  const Status status = ErrorStatus(severity);
  return Result(decision,
                status,
                MakeMGALockLifecycleDiagnostic(status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               request,
                                               wait,
                                               std::move(required_action),
                                               true));
}

bool ValidateRequestShape(const MGALockRequest& request, std::string* reason) {
  if (!LooksLikeUuidV7Text(request.request_id)) {
    *reason = "request_id";
    return false;
  }
  if (!IsValidOwnerKind(request.owner_kind) || !LooksLikeUuidText(request.owner_uuid)) {
    *reason = "owner_uuid";
    return false;
  }
  if (!IsValidResourceKind(request.resource_kind)) {
    *reason = "resource_kind";
    return false;
  }
  if (!LooksLikeUuidText(request.database_uuid)) {
    *reason = "database_uuid";
    return false;
  }
  if (!IsValidScopeKind(request.scope_kind)) {
    *reason = "scope_kind";
    return false;
  }
  if (!IsValidMode(request.mode)) {
    *reason = "mode";
    return false;
  }
  if (!IsValidWaitPolicy(request.wait_policy)) {
    *reason = "wait_policy";
    return false;
  }
  if (!IsValidPriority(request.priority_class)) {
    *reason = "priority_class";
    return false;
  }
  if (IsTransactionScopedWrite(request) &&
      (IsNone(request.transaction_uuid) || !LooksLikeUuidText(request.transaction_uuid))) {
    *reason = "transaction_uuid";
    return false;
  }
  if (request.scope_kind == MGALockScopeKind::record_lineage &&
      (IsNone(request.record_uuid) || !LooksLikeUuidText(request.record_uuid))) {
    *reason = "record_uuid";
    return false;
  }
  if ((request.scope_kind == MGALockScopeKind::record_lineage ||
       request.scope_kind == MGALockScopeKind::index ||
       request.scope_kind == MGALockScopeKind::index_key ||
       request.scope_kind == MGALockScopeKind::index_key_range) &&
      (IsNone(request.table_uuid) || !LooksLikeUuidText(request.table_uuid))) {
    *reason = "table_uuid";
    return false;
  }
  return true;
}

std::vector<std::string> BlockingGrants(const std::map<std::string, MGALockWaitLifecycle::GrantRecord>& grants,
                                        const MGALockRequest& request,
                                        const std::string& resource_key) {
  std::vector<std::string> blockers;
  for (const auto& item : grants) {
    const auto& record = item.second;
    if (record.resource_key != resource_key) { continue; }
    if (SameOwner(request, record.grant)) { continue; }
    if (!MGALockModesCompatible(request.mode, record.grant.granted_mode)) {
      blockers.push_back(record.grant.grant_id);
    }
  }
  std::sort(blockers.begin(), blockers.end());
  return blockers;
}

bool OwnerHasGrantOnResource(const std::map<std::string, MGALockWaitLifecycle::GrantRecord>& grants,
                             const MGALockRequest& request,
                             const std::string& resource_key) {
  for (const auto& item : grants) {
    const auto& record = item.second;
    if (record.resource_key == resource_key && SameOwner(request, record.grant)) {
      return true;
    }
  }
  return false;
}

u64 HeldLocksForOwner(const std::map<std::string, MGALockWaitLifecycle::GrantRecord>& grants,
                     const std::string& owner_uuid) {
  u64 count = 0;
  for (const auto& item : grants) {
    if (item.second.grant.owner_uuid == owner_uuid) { ++count; }
  }
  return count;
}

std::string MakeGrantId(const std::string& request_id, u64 generation) {
  return "grant:" + request_id + ":" + std::to_string(generation);
}

std::string MakeWaitId(const std::string& request_id, u64 sequence) {
  return "wait:" + request_id + ":" + std::to_string(sequence);
}

MGALockGrant MakeGrant(const MGALockRequest& request, u64 now_millis, u64 generation) {
  MGALockGrant grant;
  grant.grant_id = MakeGrantId(request.request_id, generation);
  grant.request_id = request.request_id;
  grant.owner_uuid = request.owner_uuid;
  grant.owner_kind = request.owner_kind;
  grant.resource_kind = request.resource_kind;
  grant.scope_kind = request.scope_kind;
  grant.scope_uuid = request.scope_uuid;
  grant.database_uuid = request.database_uuid;
  grant.granted_mode = request.mode;
  grant.granted_at_millis = now_millis;
  grant.grant_generation = generation;
  grant.release_condition = request.release_condition;
  grant.durable_recovery_source = request.durable_recovery_source;
  return grant;
}

MGALockWait MakeWait(const MGALockRequest& request,
                     std::vector<std::string> blockers,
                     u64 now_millis,
                     u64 sequence) {
  MGALockWait wait;
  wait.wait_id = MakeWaitId(request.request_id, sequence);
  wait.request_id = request.request_id;
  wait.owner_uuid = request.owner_uuid;
  wait.resource_kind = request.resource_kind;
  wait.scope_kind = request.scope_kind;
  wait.scope_uuid = request.scope_uuid;
  wait.database_uuid = request.database_uuid;
  wait.mode = request.mode;
  wait.blocked_by_grant_ids = std::move(blockers);
  wait.wait_started_at_millis = now_millis;
  wait.deadline_millis = DeadlineMillis(request);
  wait.wait_policy = request.wait_policy;
  wait.deadlock_detection_required =
      request.wait_policy == MGAWaitPolicy::wait_with_deadlock_detection ||
      request.wait_policy == MGAWaitPolicy::wait_with_priority ||
      request.distributed_wait;
  wait.distributed_wait = request.distributed_wait;
  wait.public_summary = std::string(MGAWaitResourceKindName(request.resource_kind)) + ":" +
                        MGALockScopeKindName(request.scope_kind) + ":" +
                        MGALockModeName(request.mode);
  wait.protected_summary = request.owner_uuid + " waits on " + wait.public_summary;
  return wait;
}

void RemoveWaitOrder(std::vector<std::string>* wait_order, const std::string& wait_id) {
  wait_order->erase(std::remove(wait_order->begin(), wait_order->end(), wait_id), wait_order->end());
}

std::map<std::string, std::set<std::string>> BuildWaitForGraph(
    const std::map<std::string, MGALockWaitLifecycle::GrantRecord>& grants,
    const std::map<std::string, MGALockWaitLifecycle::WaitRecord>& waits) {
  std::map<std::string, std::set<std::string>> graph;
  for (const auto& item : waits) {
    const auto& wait = item.second.wait;
    auto& edges = graph[wait.owner_uuid];
    for (const std::string& grant_id : wait.blocked_by_grant_ids) {
      const auto grant = grants.find(grant_id);
      if (grant != grants.end() && grant->second.grant.owner_uuid != wait.owner_uuid) {
        edges.insert(grant->second.grant.owner_uuid);
      }
    }
  }
  return graph;
}

bool FindCycleFrom(const std::map<std::string, std::set<std::string>>& graph,
                   const std::string& current,
                   std::vector<std::string>* path,
                   std::set<std::string>* visited,
                   std::vector<std::string>* cycle) {
  const auto in_path = std::find(path->begin(), path->end(), current);
  if (in_path != path->end()) {
    cycle->assign(in_path, path->end());
    return true;
  }
  if (visited->find(current) != visited->end()) { return false; }
  visited->insert(current);
  path->push_back(current);
  const auto edges = graph.find(current);
  if (edges != graph.end()) {
    for (const std::string& next : edges->second) {
      if (FindCycleFrom(graph, next, path, visited, cycle)) { return true; }
    }
  }
  path->pop_back();
  return false;
}

std::vector<std::string> FindFirstCycle(const std::map<std::string, std::set<std::string>>& graph) {
  for (const auto& node : graph) {
    std::vector<std::string> path;
    std::set<std::string> visited;
    std::vector<std::string> cycle;
    if (FindCycleFrom(graph, node.first, &path, &visited, &cycle)) {
      std::sort(cycle.begin(), cycle.end());
      return cycle;
    }
  }
  return {};
}

const MGALockWaitLifecycle::WaitRecord* WaitForOwner(
    const std::map<std::string, MGALockWaitLifecycle::WaitRecord>& waits,
    const std::string& owner_uuid) {
  const MGALockWaitLifecycle::WaitRecord* best = nullptr;
  for (const auto& item : waits) {
    if (item.second.wait.owner_uuid != owner_uuid) { continue; }
    if (best == nullptr || item.second.queue_sequence < best->queue_sequence) {
      best = &item.second;
    }
  }
  return best;
}

bool VictimLess(const std::map<std::string, MGALockWaitLifecycle::GrantRecord>& grants,
                const std::map<std::string, MGALockWaitLifecycle::WaitRecord>& waits,
                const std::string& left,
                const std::string& right) {
  const auto* left_wait = WaitForOwner(waits, left);
  const auto* right_wait = WaitForOwner(waits, right);
  const MGALockRequest left_request = left_wait == nullptr ? MGALockRequest{} : left_wait->request;
  const MGALockRequest right_request = right_wait == nullptr ? MGALockRequest{} : right_wait->request;
  if (left_request.recovery_critical != right_request.recovery_critical) {
    return !left_request.recovery_critical;
  }
  if (left_request.priority_class != right_request.priority_class) {
    return static_cast<u16>(left_request.priority_class) < static_cast<u16>(right_request.priority_class);
  }
  if (left_request.durable_work_completed != right_request.durable_work_completed) {
    return left_request.durable_work_completed < right_request.durable_work_completed;
  }
  if (left_request.transaction_age_millis != right_request.transaction_age_millis) {
    return left_request.transaction_age_millis < right_request.transaction_age_millis;
  }
  const u64 left_locks = left_request.locks_held_hint == 0 ? HeldLocksForOwner(grants, left)
                                                          : left_request.locks_held_hint;
  const u64 right_locks = right_request.locks_held_hint == 0 ? HeldLocksForOwner(grants, right)
                                                            : right_request.locks_held_hint;
  if (left_locks != right_locks) { return left_locks < right_locks; }
  if (left_request.retry_cost != right_request.retry_cost) {
    return left_request.retry_cost < right_request.retry_cost;
  }
  return left < right;
}

bool CycleContainsDistributedWait(const std::map<std::string, MGALockWaitLifecycle::WaitRecord>& waits,
                                  const std::vector<std::string>& cycle) {
  for (const std::string& owner : cycle) {
    const auto* wait = WaitForOwner(waits, owner);
    if (wait != nullptr && wait->wait.distributed_wait) { return true; }
  }
  return false;
}

bool HasEarlierWaitForResource(const std::vector<std::string>& wait_order,
                               const std::map<std::string, MGALockWaitLifecycle::WaitRecord>& waits,
                               const MGALockWaitLifecycle::WaitRecord& candidate) {
  for (const std::string& wait_id : wait_order) {
    if (wait_id == candidate.wait.wait_id) { return false; }
    const auto found = waits.find(wait_id);
    if (found != waits.end() && found->second.resource_key == candidate.resource_key) {
      return true;
    }
  }
  return false;
}

}  // namespace

MGALockLifecycleResult MGALockWaitLifecycle::Acquire(MGALockRequest request, u64 now_millis) {
  ++metrics_.lock_requests;
  std::string invalid_reason;
  if (!ValidateRequestShape(request, &invalid_reason)) {
    request.scope_uuid = invalid_reason;
    return ErrorResult(MGALockLifecycleDecision::invalid_request,
                       "diag.mga.lock.invalid_request",
                       "mga.lock.invalid_request",
                       &request,
                       nullptr,
                       "reject_request_shape",
                       Severity::fatal);
  }
  if ((IsClusterScope(request.scope_kind) || IsClusterMode(request.mode)) &&
      (!request.cluster_exists || IsNone(request.cluster_uuid))) {
    return ErrorResult(MGALockLifecycleDecision::cluster_absent,
                       "diag.mga.lock.cluster_absent",
                       "mga.lock.cluster_absent",
                       &request,
                       nullptr,
                       "reject_without_cluster_state",
                       Severity::fatal);
  }
  if (!accepting_new_requests_) {
    return ErrorResult(MGALockLifecycleDecision::admission_rejected,
                       "diag.mga.lock.admission_rejected_policy",
                       "mga.lock.admission_rejected_policy",
                       &request,
                       nullptr,
                       "retry_after_shutdown_or_reopen");
  }

  const std::string resource_key = ResourceKey(request);
  if (OwnerHasGrantOnResource(grants_, request, resource_key)) {
    return Result(MGALockLifecycleDecision::already_owned, OkStatus());
  }

  const std::vector<std::string> blockers = BlockingGrants(grants_, request, resource_key);
  if (blockers.empty()) {
    const u64 generation = ++generation_;
    GrantRecord record;
    record.grant = MakeGrant(request, now_millis, generation);
    record.request = request;
    record.resource_key = resource_key;
    MGALockLifecycleResult result = Result(MGALockLifecycleDecision::granted, OkStatus());
    result.grant = record.grant;
    grants_.emplace(record.grant.grant_id, record);
    ++metrics_.lock_grants;
    return result;
  }

  if (request.wait_policy == MGAWaitPolicy::no_wait) {
    return ErrorResult(MGALockLifecycleDecision::lock_refused,
                       "diag.mga.lock.write_intent_conflict",
                       "mga.lock.write_intent_conflict",
                       &request,
                       nullptr,
                       "return_lock_refusal_without_wait");
  }
  if (DeadlineExpired(request, now_millis)) {
    ++metrics_.lock_timeouts;
    return ErrorResult(MGALockLifecycleDecision::wait_timeout,
                       "diag.mga.lock.wait_timeout",
                       "mga.lock.wait_timeout",
                       &request,
                       nullptr,
                       "return_timeout_transaction_state_unchanged");
  }

  WaitRecord wait_record;
  wait_record.request = request;
  wait_record.resource_key = resource_key;
  wait_record.queue_sequence = ++wait_sequence_;
  wait_record.wait = MakeWait(request, blockers, now_millis, wait_record.queue_sequence);
  waits_[wait_record.wait.wait_id] = wait_record;
  wait_order_.push_back(wait_record.wait.wait_id);
  ++metrics_.lock_waits;

  if (wait_record.wait.deadlock_detection_required) {
    MGALockLifecycleResult deadlock = DetectAndResolveDeadlock(now_millis);
    if (deadlock.decision == MGALockLifecycleDecision::deadlock_victim &&
        deadlock.victim_owner_uuid == request.owner_uuid) {
      return deadlock;
    }
    if (deadlock.decision == MGALockLifecycleDecision::cluster_decision_required) {
      return deadlock;
    }
  }

  MGALockLifecycleResult result = Result(MGALockLifecycleDecision::wait_queued, ErrorStatus());
  result.wait = waits_[wait_record.wait.wait_id].wait;
  result.diagnostic = MakeMGALockLifecycleDiagnostic(result.status,
                                                    "diag.mga.lock.write_intent_conflict",
                                                    "mga.lock.wait_queued",
                                                    &request,
                                                    &result.wait,
                                                    "wait_for_incompatible_owner_release",
                                                    true);
  return result;
}

MGALockLifecycleResult MGALockWaitLifecycle::ReleaseGrant(std::string grant_id, u64 now_millis) {
  const auto found = grants_.find(grant_id);
  if (found == grants_.end()) {
    MGALockRequest request;
    request.request_id = "none";
    return ErrorResult(MGALockLifecycleDecision::invalid_request,
                       "diag.mga.lock.invalid_request",
                       "mga.lock.invalid_release",
                       &request,
                       nullptr,
                       "reject_unknown_grant");
  }
  grants_.erase(found);
  DrainGrantableWaits(now_millis);
  MGALockLifecycleResult result = Result(MGALockLifecycleDecision::released, OkStatus());
  result.cleanup_count = 1;
  return result;
}

MGALockLifecycleResult MGALockWaitLifecycle::ReleaseOwner(std::string owner_uuid, u64 now_millis) {
  u64 cleaned = 0;
  for (auto it = grants_.begin(); it != grants_.end();) {
    if (it->second.grant.owner_uuid == owner_uuid) {
      it = grants_.erase(it);
      ++cleaned;
    } else {
      ++it;
    }
  }
  for (auto it = waits_.begin(); it != waits_.end();) {
    if (it->second.wait.owner_uuid == owner_uuid) {
      RemoveWaitOrder(&wait_order_, it->first);
      it = waits_.erase(it);
      ++cleaned;
    } else {
      ++it;
    }
  }
  DrainGrantableWaits(now_millis);
  MGALockLifecycleResult result = Result(MGALockLifecycleDecision::released, OkStatus());
  result.cleanup_count = cleaned;
  return result;
}

MGALockLifecycleResult MGALockWaitLifecycle::CancelWait(std::string wait_id, u64 now_millis) {
  (void)now_millis;
  const auto found = waits_.find(wait_id);
  if (found == waits_.end()) {
    MGALockRequest request;
    request.request_id = "none";
    return ErrorResult(MGALockLifecycleDecision::invalid_request,
                       "diag.mga.lock.invalid_request",
                       "mga.lock.invalid_cancel",
                       &request,
                       nullptr,
                       "reject_unknown_wait");
  }
  MGALockWait wait = found->second.wait;
  MGALockRequest request = found->second.request;
  waits_.erase(found);
  RemoveWaitOrder(&wait_order_, wait_id);
  ++metrics_.lock_cancellations;
  MGALockLifecycleResult result = ErrorResult(MGALockLifecycleDecision::wait_cancelled,
                                              "diag.mga.lock.admission_rejected_policy",
                                              "mga.lock.wait_cancelled",
                                              &request,
                                              &wait,
                                              "return_cancelled_transaction_state_unchanged");
  result.wait = wait;
  return result;
}

MGALockLifecycleResult MGALockWaitLifecycle::DisconnectOwner(std::string owner_uuid, u64 now_millis) {
  MGALockLifecycleResult result = ReleaseOwner(std::move(owner_uuid), now_millis);
  result.decision = MGALockLifecycleDecision::owner_disconnected;
  ++metrics_.disconnect_cleanups;
  return result;
}

std::vector<MGALockLifecycleResult> MGALockWaitLifecycle::ProcessTimeouts(u64 now_millis) {
  std::vector<MGALockLifecycleResult> expired;
  for (auto it = waits_.begin(); it != waits_.end();) {
    if (!WaitDeadlineExpired(it->second.wait, now_millis)) {
      ++it;
      continue;
    }
    MGALockWait wait = it->second.wait;
    MGALockRequest request = it->second.request;
    RemoveWaitOrder(&wait_order_, it->first);
    it = waits_.erase(it);
    ++metrics_.lock_timeouts;
    MGALockLifecycleResult result = ErrorResult(MGALockLifecycleDecision::wait_timeout,
                                                "diag.mga.lock.wait_timeout",
                                                "mga.lock.wait_timeout",
                                                &request,
                                                &wait,
                                                "return_timeout_transaction_state_unchanged");
    result.wait = std::move(wait);
    expired.push_back(std::move(result));
  }
  DrainGrantableWaits(now_millis);
  return expired;
}

MGALockLifecycleResult MGALockWaitLifecycle::DetectAndResolveDeadlock(u64 now_millis) {
  (void)now_millis;
  const auto graph = BuildWaitForGraph(grants_, waits_);
  const std::vector<std::string> cycle = FindFirstCycle(graph);
  if (cycle.empty()) {
    MGALockLifecycleResult result = Result(MGALockLifecycleDecision::deadlock_detected, OkStatus());
    result.cleanup_count = 0;
    return result;
  }

  ++metrics_.deadlocks_detected;
  if (CycleContainsDistributedWait(waits_, cycle)) {
    const auto* wait = WaitForOwner(waits_, cycle.front());
    const MGALockRequest* request = wait == nullptr ? nullptr : &wait->request;
    const MGALockWait* wait_record = wait == nullptr ? nullptr : &wait->wait;
    return ErrorResult(MGALockLifecycleDecision::cluster_decision_required,
                       "diag.mga.lock.cluster_decision_required",
                       "mga.lock.cluster_decision_required",
                       request,
                       wait_record,
                       "request_cluster_distributed_deadlock_victim",
                       Severity::fatal);
  }

  std::string victim = cycle.front();
  for (const std::string& owner : cycle) {
    if (VictimLess(grants_, waits_, owner, victim)) { victim = owner; }
  }
  const auto* victim_wait = WaitForOwner(waits_, victim);
  if (victim_wait == nullptr) {
    return ErrorResult(MGALockLifecycleDecision::deadlock_detected,
                       "diag.mga.lock.deadlock_detected",
                       "mga.lock.deadlock_detected",
                       nullptr,
                       nullptr,
                       "operator_review_deadlock_without_wait_record",
                       Severity::fatal);
  }

  const std::string wait_id = victim_wait->wait.wait_id;
  const MGALockWait wait = victim_wait->wait;
  const MGALockRequest request = victim_wait->request;
  waits_.erase(wait_id);
  RemoveWaitOrder(&wait_order_, wait_id);
  ++metrics_.deadlock_victims;

  MGALockLifecycleResult result = ErrorResult(MGALockLifecycleDecision::deadlock_victim,
                                              "diag.mga.lock.deadlock_victim",
                                              "mga.lock.deadlock_victim",
                                              &request,
                                              &wait,
                                              "return_deadlock_victim_transaction_state_unchanged",
                                              Severity::error);
  result.wait = wait;
  result.victim_owner_uuid = victim;
  result.victim_wait_id = wait_id;

  MGADeadlockRecord record;
  record.cycle_owner_uuids = cycle;
  record.victim_owner_uuid = victim;
  record.victim_wait_id = wait_id;
  record.distributed = false;
  record.diagnostic = MakeMGALockLifecycleDiagnostic(ErrorStatus(Severity::fatal),
                                                     "diag.mga.lock.deadlock_detected",
                                                     "mga.lock.deadlock_detected",
                                                     &request,
                                                     &wait,
                                                     "selected_deterministic_local_victim",
                                                     true);
  deadlock_history_.push_back(std::move(record));
  return result;
}

MGALockLifecycleResult MGALockWaitLifecycle::Shutdown(u64 now_millis) {
  (void)now_millis;
  accepting_new_requests_ = false;
  const u64 cleaned = static_cast<u64>(grants_.size() + waits_.size());
  grants_.clear();
  waits_.clear();
  wait_order_.clear();
  ++metrics_.shutdown_cleanups;
  MGALockLifecycleResult result = Result(MGALockLifecycleDecision::shutdown_cleanup, OkStatus());
  result.cleanup_count = cleaned;
  return result;
}

u64 MGALockWaitLifecycle::grant_count() const {
  return static_cast<u64>(grants_.size());
}

u64 MGALockWaitLifecycle::wait_count() const {
  return static_cast<u64>(waits_.size());
}

MGALockLifecycleMetrics MGALockWaitLifecycle::metrics() const {
  return metrics_;
}

std::vector<MGALockGrant> MGALockWaitLifecycle::grants() const {
  std::vector<MGALockGrant> out;
  for (const auto& item : grants_) { out.push_back(item.second.grant); }
  return out;
}

std::vector<MGALockWait> MGALockWaitLifecycle::waits() const {
  std::vector<MGALockWait> out;
  for (const std::string& wait_id : wait_order_) {
    const auto found = waits_.find(wait_id);
    if (found != waits_.end()) { out.push_back(found->second.wait); }
  }
  return out;
}

std::vector<MGADeadlockRecord> MGALockWaitLifecycle::deadlock_history() const {
  return deadlock_history_;
}

std::vector<MGALockGrant> MGALockWaitLifecycle::DrainGrantableWaits(u64 now_millis) {
  std::vector<MGALockGrant> granted;
  bool progressed = true;
  while (progressed) {
    progressed = false;
    for (const std::string& wait_id : std::vector<std::string>(wait_order_)) {
      const auto found = waits_.find(wait_id);
      if (found == waits_.end()) { continue; }
      if (HasEarlierWaitForResource(wait_order_, waits_, found->second)) { continue; }
      const std::vector<std::string> blockers =
          BlockingGrants(grants_, found->second.request, found->second.resource_key);
      if (!blockers.empty()) {
        waits_[wait_id].wait.blocked_by_grant_ids = blockers;
        continue;
      }
      GrantRecord record;
      record.request = found->second.request;
      record.resource_key = found->second.resource_key;
      record.grant = MakeGrant(record.request, now_millis, ++generation_);
      granted.push_back(record.grant);
      grants_.emplace(record.grant.grant_id, record);
      waits_.erase(wait_id);
      RemoveWaitOrder(&wait_order_, wait_id);
      ++metrics_.lock_grants;
      progressed = true;
      break;
    }
  }
  return granted;
}

const char* MGAWaitResourceKindName(MGAWaitResourceKind resource_kind) {
  switch (resource_kind) {
    case MGAWaitResourceKind::logical_lock: return "logical_lock";
    case MGAWaitResourceKind::physical_latch: return "physical_latch";
    case MGAWaitResourceKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGALockOwnerKindName(MGALockOwnerKind owner_kind) {
  switch (owner_kind) {
    case MGALockOwnerKind::transaction: return "transaction";
    case MGALockOwnerKind::statement: return "statement";
    case MGALockOwnerKind::cursor: return "cursor";
    case MGALockOwnerKind::operation: return "operation";
    case MGALockOwnerKind::recovery: return "recovery";
    case MGALockOwnerKind::cleanup: return "cleanup";
    case MGALockOwnerKind::archive: return "archive";
    case MGALockOwnerKind::backup_restore: return "backup_restore";
    case MGALockOwnerKind::cluster_participant: return "cluster_participant";
    case MGALockOwnerKind::engine_coordinator: return "engine_coordinator";
    case MGALockOwnerKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGALockScopeKindName(MGALockScopeKind scope_kind) {
  switch (scope_kind) {
    case MGALockScopeKind::database: return "database";
    case MGALockScopeKind::filespace: return "filespace";
    case MGALockScopeKind::page: return "page";
    case MGALockScopeKind::relation: return "relation";
    case MGALockScopeKind::record_lineage: return "record_lineage";
    case MGALockScopeKind::record_version: return "record_version";
    case MGALockScopeKind::index: return "index";
    case MGALockScopeKind::index_key: return "index_key";
    case MGALockScopeKind::index_key_range: return "index_key_range";
    case MGALockScopeKind::predicate_guard: return "predicate_guard";
    case MGALockScopeKind::catalog_object_uuid: return "catalog_object_uuid";
    case MGALockScopeKind::metadata_name_binding: return "metadata_name_binding";
    case MGALockScopeKind::domain_object: return "domain_object";
    case MGALockScopeKind::udr_package: return "udr_package";
    case MGALockScopeKind::security_policy_generation: return "security_policy_generation";
    case MGALockScopeKind::archive_descriptor: return "archive_descriptor";
    case MGALockScopeKind::backup_boundary: return "backup_boundary";
    case MGALockScopeKind::cluster_transaction_identifier: return "cluster_transaction_identifier";
    case MGALockScopeKind::cluster_member_epoch: return "cluster_member_epoch";
    case MGALockScopeKind::cluster_routing_or_placement_unit: return "cluster_routing_or_placement_unit";
    case MGALockScopeKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGALockModeName(MGALockMode mode) {
  switch (mode) {
    case MGALockMode::intent_shared: return "intent_shared";
    case MGALockMode::intent_exclusive: return "intent_exclusive";
    case MGALockMode::shared_read: return "shared_read";
    case MGALockMode::shared_stability: return "shared_stability";
    case MGALockMode::update_intent: return "update_intent";
    case MGALockMode::exclusive_write: return "exclusive_write";
    case MGALockMode::schema_stability: return "schema_stability";
    case MGALockMode::schema_modify: return "schema_modify";
    case MGALockMode::range_shared: return "range_shared";
    case MGALockMode::range_update: return "range_update";
    case MGALockMode::range_insert_guard: return "range_insert_guard";
    case MGALockMode::range_exclusive: return "range_exclusive";
    case MGALockMode::maintenance_shared: return "maintenance_shared";
    case MGALockMode::maintenance_exclusive: return "maintenance_exclusive";
    case MGALockMode::recovery_exclusive: return "recovery_exclusive";
    case MGALockMode::archive_shared: return "archive_shared";
    case MGALockMode::archive_exclusive: return "archive_exclusive";
    case MGALockMode::cluster_prepare: return "cluster_prepare";
    case MGALockMode::cluster_decision: return "cluster_decision";
    case MGALockMode::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGAWaitPolicyName(MGAWaitPolicy policy) {
  switch (policy) {
    case MGAWaitPolicy::no_wait: return "no_wait";
    case MGAWaitPolicy::wait_forever: return "wait_forever";
    case MGAWaitPolicy::wait_timeout: return "wait_timeout";
    case MGAWaitPolicy::wait_until_statement_end: return "wait_until_statement_end";
    case MGAWaitPolicy::wait_until_transaction_end: return "wait_until_transaction_end";
    case MGAWaitPolicy::wait_with_deadlock_detection: return "wait_with_deadlock_detection";
    case MGAWaitPolicy::wait_with_priority: return "wait_with_priority";
    case MGAWaitPolicy::reference_compatible_wait: return "reference_compatible_wait";
    case MGAWaitPolicy::maintenance_window_wait: return "maintenance_window_wait";
    case MGAWaitPolicy::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGAPriorityClassName(MGAPriorityClass priority_class) {
  switch (priority_class) {
    case MGAPriorityClass::normal: return "normal";
    case MGAPriorityClass::foreground: return "foreground";
    case MGAPriorityClass::maintenance: return "maintenance";
    case MGAPriorityClass::recovery: return "recovery";
    case MGAPriorityClass::emergency: return "emergency";
    case MGAPriorityClass::cluster_decision: return "cluster_decision";
    case MGAPriorityClass::unknown: return "unknown";
  }
  return "unknown";
}

const char* MGALockLifecycleDecisionName(MGALockLifecycleDecision decision) {
  switch (decision) {
    case MGALockLifecycleDecision::granted: return "granted";
    case MGALockLifecycleDecision::already_owned: return "already_owned";
    case MGALockLifecycleDecision::wait_queued: return "wait_queued";
    case MGALockLifecycleDecision::lock_refused: return "lock_refused";
    case MGALockLifecycleDecision::wait_timeout: return "wait_timeout";
    case MGALockLifecycleDecision::wait_cancelled: return "wait_cancelled";
    case MGALockLifecycleDecision::owner_disconnected: return "owner_disconnected";
    case MGALockLifecycleDecision::deadlock_detected: return "deadlock_detected";
    case MGALockLifecycleDecision::deadlock_victim: return "deadlock_victim";
    case MGALockLifecycleDecision::released: return "released";
    case MGALockLifecycleDecision::shutdown_cleanup: return "shutdown_cleanup";
    case MGALockLifecycleDecision::admission_rejected: return "admission_rejected";
    case MGALockLifecycleDecision::cluster_absent: return "cluster_absent";
    case MGALockLifecycleDecision::cluster_decision_required: return "cluster_decision_required";
    case MGALockLifecycleDecision::invalid_request: return "invalid_request";
  }
  return "invalid_request";
}

bool MGALockModesCompatible(MGALockMode requested, MGALockMode existing) {
  static constexpr bool kMatrix[19][19] = {
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {true, true, false, true, true, false, true, false, false, true, true, false, true, false, false, true, false, true, false},
      {true, false, true, true, false, false, true, false, true, false, true, false, true, false, false, true, false, false, false},
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {true, true, false, true, false, false, true, false, false, false, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {true, false, true, true, false, false, true, false, true, false, true, false, true, false, false, true, false, false, false},
      {true, true, false, true, false, false, true, false, false, false, true, false, true, false, false, true, false, true, false},
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {true, true, true, true, true, false, true, false, true, true, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
      {true, true, false, true, true, false, true, false, false, true, true, false, true, false, false, true, false, true, false},
      {false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false},
  };
  if (requested == MGALockMode::unknown || existing == MGALockMode::unknown) { return false; }
  return kMatrix[ModeIndex(requested)][ModeIndex(existing)];
}

DiagnosticRecord MakeMGALockLifecycleDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                const MGALockRequest* request,
                                                const MGALockWait* wait,
                                                std::string required_action,
                                                bool protected_detail) {
  std::vector<DiagnosticArgument> args;
  args.push_back({"diag_code", diagnostic_code});
  args.push_back({"severity", SeverityName(status.severity)});
  args.push_back({"request_id", request == nullptr ? "none" : request->request_id});
  args.push_back({"owner_uuid", request == nullptr ? "none" : request->owner_uuid});
  args.push_back({"resource_kind", request == nullptr ? "none" : MGAWaitResourceKindName(request->resource_kind)});
  args.push_back({"scope_kind", request == nullptr ? "none" : MGALockScopeKindName(request->scope_kind)});
  args.push_back({"scope_uuid", request == nullptr ? "none" : request->scope_uuid});
  args.push_back({"mode", request == nullptr ? "none" : MGALockModeName(request->mode)});
  args.push_back({"wait_id", wait == nullptr ? "none" : wait->wait_id});
  args.push_back({"cluster_epoch", "none"});
  args.push_back({"route_generation",
                  request != nullptr && request->route_generation_present
                      ? std::to_string(request->route_generation)
                      : "none"});
  args.push_back({"required_action", required_action.empty() ? "operator_review" : required_action});
  args.push_back({"protected", protected_detail ? "true" : "false"});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(args),
                        {},
                        "transaction.mga.lock_wait_lifecycle");
}

}  // namespace scratchbird::transaction::mga
