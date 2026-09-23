// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog/sys_information_projection.hpp"
#include "security/security_principal_lifecycle.hpp"
#include <set>

namespace scratchbird::engine::internal_api {

// Display projection only: the durable security owner supplies membership and
// name authority. Text request options cannot introduce principal identities.
inline void PopulateSysInformationSecurityContext(
    const EngineRequestContext& request, SysInformationProjectionContext* projection) {
  projection->principal_uuid = request.principal_uuid;
  projection->active_role_uuid = request.current_role_uuid;
  if (request.principal_uuid.is_nil()) { return; }
  const auto loaded = LoadSecurityPrincipalLifecycleState(request);
  if (!loaded.ok) {
    projection->source_diagnostic_code = loaded.diagnostic.code;
    projection->source_diagnostic_detail = "security catalog projection unavailable";
    return;
  }
  const auto& security = loaded.state;
  for (const auto& principal : security.principals) {
    if (!principal.deleted && principal.lifecycle_state == "active" &&
        principal.principal_uuid == request.principal_uuid) {
      projection->principal_name = principal.principal_name;
    }
  }
  std::set<EngineUuid> subjects{request.principal_uuid};
  std::set<EngineUuid> groups;
  std::set<EngineUuid> roles;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& membership : security.memberships) {
      if (membership.revoked || membership.container_uuid.is_nil() ||
          subjects.count(membership.member_principal_uuid) == 0) { continue; }
      bool active = false;
      if (membership.container_kind == "group") {
        for (const auto& group : security.groups) {
          if (group.group_uuid == membership.container_uuid && !group.deleted &&
              group.lifecycle_state == "active") { active = true; groups.insert(group.group_uuid); }
        }
      } else if (membership.container_kind == "role") {
        for (const auto& role : security.roles) {
          if (role.role_uuid == membership.container_uuid && !role.deleted &&
              role.lifecycle_state == "active") { active = true; roles.insert(role.role_uuid); }
        }
      }
      if (active && subjects.insert(membership.container_uuid).second) { changed = true; }
    }
  }
  for (const auto& role : security.roles) {
    if (role.deleted || role.lifecycle_state != "active") { continue; }
    if (role.role_uuid == request.current_role_uuid) {
      projection->active_role_name = role.role_name;
      roles.insert(role.role_uuid);
    }
  }
  projection->effective_role_uuids.assign(roles.begin(), roles.end());
  projection->effective_group_uuids.assign(groups.begin(), groups.end());
  projection->effective_role_names.clear();
  for (const auto& uuid : projection->effective_role_uuids) {
    for (const auto& role : security.roles) {
      if (role.role_uuid == uuid && !role.deleted && role.lifecycle_state == "active") {
        projection->effective_role_names.push_back(role.role_name);
        break;
      }
    }
  }
}

}  // namespace scratchbird::engine::internal_api
