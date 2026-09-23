// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <initializer_list>
#include <string>

namespace scratchbird::tests::release {

inline void GrantMaterializedRight(
    scratchbird::engine::internal_api::EngineRequestContext* context,
    const std::string& right,
    const scratchbird::engine::internal_api::EngineUuid& target_uuid = {}) {
  if (context == nullptr) {
    return;
  }
  context->security_context_present = true;
  if (context->security_epoch == 0) {
    context->security_epoch = 1;
  }
  if (context->catalog_generation_id == 0) {
    context->catalog_generation_id = 1;
  }

  auto& authz = context->authorization_context;
  authz.present = true;
  if (authz.security_context_generation == 0) {
    authz.security_context_generation = 1;
  }
  if (authz.authority_uuid.is_nil()) {
    authz.authority_uuid =
        FixtureUuid(0xfeed, 1);
  }
  authz.principal_uuid = context->principal_uuid;
  authz.security_epoch = context->security_epoch;
  authz.policy_epoch = authz.policy_epoch == 0 ? 1 : authz.policy_epoch;
  authz.catalog_generation_id = context->catalog_generation_id;
  if (authz.effective_subjects.empty()) {
    authz.effective_subjects.push_back({context->principal_uuid, "principal"});
  }

  scratchbird::engine::internal_api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid = FixtureUuid(0xfeed, static_cast<std::uint32_t>(authz.grants.size()) + 2);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid = target_uuid;
  grant.right = right;
  grant.security_epoch = context->security_epoch;
  authz.grants.push_back(std::move(grant));

  bool has_evidence = false;
  for (const auto& tag : authz.evidence_tags) {
    if (tag == "durable_authorization_context") {
      has_evidence = true;
      break;
    }
  }
  if (!has_evidence) {
    authz.evidence_tags.push_back("durable_authorization_context");
  }
}

inline void GrantMaterializedRights(
    scratchbird::engine::internal_api::EngineRequestContext* context,
    std::initializer_list<const char*> rights) {
  for (const char* right : rights) {
    GrantMaterializedRight(context, right == nullptr ? std::string{} : right);
  }
}

}  // namespace scratchbird::tests::release
