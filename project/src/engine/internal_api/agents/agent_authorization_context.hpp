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

#include <algorithm>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace agent_authorization {

inline void AddUnique(std::vector<std::string>* values, std::string value) {
  if (value.empty() ||
      std::find(values->begin(), values->end(), value) != values->end()) {
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
        return subject.subject_uuid.canonical == subject_uuid.canonical &&
               subject.subject_kind == subject_kind;
      });
}

inline bool MaterializedContextMatchesRequest(
    const EngineRequestContext& request) {
  const auto& authorization = request.authorization_context;
  return request.security_context_present && authorization.present &&
         !request.principal_uuid.canonical.empty() &&
         authorization.principal_uuid.canonical ==
             request.principal_uuid.canonical &&
         authorization.security_epoch != 0 &&
         authorization.policy_epoch != 0 &&
         authorization.catalog_generation_id != 0 &&
         (request.security_epoch == 0 ||
          request.security_epoch == authorization.security_epoch) &&
         (request.catalog_generation_id == 0 ||
          request.catalog_generation_id ==
              authorization.catalog_generation_id);
}

inline bool GrantTargetAppliesToRequest(const EngineRequestContext& request,
                                        const EngineUuid& target_uuid) {
  return target_uuid.canonical.empty() ||
         target_uuid.canonical == request.database_uuid.canonical ||
         target_uuid.canonical == request.cluster_uuid.canonical;
}

// Converts the engine-owned materialized authorization context into the
// legacy agent runtime shape. Trace tags remain evidence only in production;
// their authority bridge is retained solely for explicitly marked embedded
// fixtures.
inline void PopulateAgentRuntimeSecurityContext(
    const EngineRequestContext& request,
    scratchbird::core::agents::AgentRuntimeContext* runtime) {
  if (runtime == nullptr) return;

  const bool fixture_trace_authority =
      SecurityTraceAuthorizationFallbackAllowed(request);
  runtime->fixture_authorization_authority = fixture_trace_authority;
  std::vector<std::string> fixture_denies;
  for (const auto& tag : request.trace_tags) {
    const bool right_tag = tag.rfind("right:", 0) == 0;
    const bool group_tag = tag.rfind("group:", 0) == 0;
    const bool deny_tag = tag.rfind("deny:", 0) == 0;
    if (!right_tag && !group_tag && !deny_tag) {
      AddUnique(&runtime->trace_tags, tag);
      continue;
    }
    if (!fixture_trace_authority) continue;
    AddUnique(&runtime->trace_tags, tag);
    if (right_tag) {
      AddUnique(&runtime->rights, tag.substr(6));
    } else if (group_tag) {
      AddUnique(&runtime->groups, tag.substr(6));
    } else {
      AddUnique(&fixture_denies, tag.substr(5));
    }
  }
  for (const auto& denied : fixture_denies) {
    runtime->rights.erase(
        std::remove(runtime->rights.begin(), runtime->rights.end(), denied),
        runtime->rights.end());
    runtime->trace_tags.erase(
        std::remove(runtime->trace_tags.begin(),
                    runtime->trace_tags.end(),
                    "right:" + denied),
        runtime->trace_tags.end());
  }

  if (!MaterializedContextMatchesRequest(request)) return;
  const auto& authorization = request.authorization_context;
  for (const auto& subject : authorization.effective_subjects) {
    if (subject.subject_kind == "group") {
      // UUIDs are identity; mutable display names such as ROOT are not.
      AddUnique(&runtime->groups, subject.subject_uuid.canonical);
    }
  }

  std::vector<std::string> candidate_rights;
  for (const auto& grant : authorization.grants) {
    if (grant.deny || grant.right.empty() ||
        !GrantTargetAppliesToRequest(request, grant.target_uuid) ||
        !IsEffectiveSubject(authorization,
                            grant.subject_uuid,
                            grant.subject_kind)) {
      continue;
    }
    AddUnique(&candidate_rights, grant.right);
  }

  for (const auto& right : candidate_rights) {
    std::vector<std::string> evaluation_targets;
    if (!request.database_uuid.canonical.empty()) {
      evaluation_targets.push_back(request.database_uuid.canonical);
    }
    if (!request.cluster_uuid.canonical.empty() &&
        request.cluster_uuid.canonical != request.database_uuid.canonical) {
      evaluation_targets.push_back(request.cluster_uuid.canonical);
    }
    if (evaluation_targets.empty()) evaluation_targets.emplace_back();

    bool allowed = false;
    bool refused = false;
    for (const auto& target : evaluation_targets) {
      const auto decision = EvaluateMaterializedAuthorization(
          request, authorization, right, target);
      if (decision.denied || decision.policy_recheck_required) {
        refused = true;
        break;
      }
      allowed = allowed || decision.authorized;
    }
    if (allowed && !refused) AddUnique(&runtime->rights, right);
  }
}

}  // namespace agent_authorization
}  // namespace scratchbird::engine::internal_api
