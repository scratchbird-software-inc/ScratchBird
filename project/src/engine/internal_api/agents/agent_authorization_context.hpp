// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace agent_authorization {

enum class Scope { local_node, cluster };

inline const EngineUuid* AuthorizationTarget(const EngineRequestContext& request,
                                             Scope scope) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(request.database_uuid)) return nullptr;
  switch (scope) {
    case Scope::local_node: return &request.database_uuid;
    case Scope::cluster:
      // No cluster identity is invented when no provider/cluster is installed.
      // Evaluating a nil target accepts global grants only, leaving provider
      // availability to the owning gateway after permission checks.
      if (!request.cluster_uuid.is_nil() &&
          !scratchbird::core::uuid::IsEngineIdentityUuid(request.cluster_uuid)) return nullptr;
      return &request.cluster_uuid;
  }
  return nullptr;
}

inline bool RightAllowed(const EngineRequestContext& request,
                          Scope scope,
                          const std::string& right) {
  const auto* target = AuthorizationTarget(request, scope);
  if (target == nullptr) return false;
  const auto decision = EvaluateMaterializedAuthorization(
      request, request.authorization_context, right, *target);
  return decision.authorized && !decision.denied && !decision.policy_recheck_required;
}

template <typename Value>
inline void AddUnique(std::vector<Value>* values, Value value) {
  if (std::find(values->begin(), values->end(), value) != values->end()) {
    return;
  }
  values->push_back(std::move(value));
}

inline bool IsEffectiveSubject(
    const EngineMaterializedAuthorizationContext& authorization,
    const EngineUuid& subject_uuid,
    const std::string& subject_kind) {
  return std::any_of(
      authorization.effective_subjects.begin(),
      authorization.effective_subjects.end(),
      [&](const EngineAuthorizationSubject& subject) {
        return subject.subject_uuid == subject_uuid &&
               subject.subject_kind == subject_kind;
      });
}

inline bool MaterializedContextMatchesRequest(
    const EngineRequestContext& request) {
  const auto& authorization = request.authorization_context;
  return request.security_context_present && authorization.present &&
         scratchbird::core::uuid::IsEngineIdentityUuid(request.principal_uuid) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(authorization.authority_uuid) &&
         authorization.security_context_generation != 0 &&
         authorization.principal_uuid ==
             request.principal_uuid &&
         authorization.security_epoch != 0 &&
         authorization.policy_epoch != 0 &&
         authorization.catalog_generation_id != 0 &&
         (request.security_epoch == 0 ||
          request.security_epoch == authorization.security_epoch) &&
         (request.catalog_generation_id == 0 ||
          request.catalog_generation_id ==
              authorization.catalog_generation_id);
}

// Publish a fresh engine-owned authorization projection. Reusing an output
// context must not retain an earlier principal's grants. Trace and fixture
// strings are evidence, never authorization authority. Allocation failure
// leaves the output without grants rather than publishing a partial projection.
inline void PopulateAgentRuntimeSecurityContext(
    const EngineRequestContext& request,
    scratchbird::core::agents::AgentRuntimeContext* runtime,
    Scope scope) {
  if (runtime == nullptr) return;

  runtime->security_context_present = false;
  runtime->fixture_authorization_authority = false;
  runtime->rights.clear();
  runtime->groups.clear();
  runtime->effective_group_uuids.clear();
  runtime->trace_tags.clear();
  runtime->authorization_target_uuid = {};
  runtime->authorization_cluster_scope = false;
  std::vector<std::string> trace_tags;
  std::vector<std::string> rights;
  std::vector<EngineUuid> groups;
  for (const auto& tag : request.trace_tags) {
    const bool right_tag = tag.rfind("right:", 0) == 0;
    const bool group_tag = tag.rfind("group:", 0) == 0;
    const bool deny_tag = tag.rfind("deny:", 0) == 0;
    if (!right_tag && !group_tag && !deny_tag) {
      if (!tag.empty()) AddUnique(&trace_tags, tag);
      continue;
    }
  }

  if (!MaterializedContextMatchesRequest(request)) return;
  const auto* target = AuthorizationTarget(request, scope);
  if (target == nullptr) return;
  const auto& authorization = request.authorization_context;
  for (const auto& subject : authorization.effective_subjects) {
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(subject.subject_uuid)) return;
    if (subject.subject_kind == "group") {
      // UUIDs are identity; mutable display names such as ROOT are not.
      AddUnique(&groups, subject.subject_uuid);
    }
  }

  std::vector<std::string> candidate_rights;
  for (const auto& grant : authorization.grants) {
    if (grant.deny || grant.right.empty() ||
        (!grant.target_uuid.is_nil() && grant.target_uuid != *target) ||
        !IsEffectiveSubject(authorization,
                            grant.subject_uuid,
                            grant.subject_kind)) {
      continue;
    }
    AddUnique(&candidate_rights, grant.right);
  }
  if (!authorization.engine_owned_bootstrap_role_uuid.is_nil()) {
    // The evaluator validates the immutable bootstrap role and all explicit
    // denies/policies. Do not fabricate grant records to enumerate its rights.
    for (const auto& right : KnownSecurityRights()) AddUnique(&candidate_rights, right);
  }

  for (const auto& right : candidate_rights) {
    if (RightAllowed(request, scope, right)) AddUnique(&rights, right);
  }
  runtime->rights.swap(rights);
  runtime->effective_group_uuids.swap(groups);
  runtime->trace_tags.swap(trace_tags);
  runtime->authorization_target_uuid = *target;
  runtime->authorization_cluster_scope = scope == Scope::cluster;
  runtime->security_context_present = true;
}

}  // namespace agent_authorization
}  // namespace scratchbird::engine::internal_api
