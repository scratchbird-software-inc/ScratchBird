// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_policy_descriptor.hpp"

namespace scratchbird::engine::internal_api {
enum class NativeStartupPolicyOutcome {allow,deny,defer,require_revalidation,require_human_review,quarantine};
// Retained policy data shared by the evaluator and durable evidence codec.
// It does not construct source custody, authorization or an executable grant.
struct NativeStartupPolicyBinding {
  core::platform::Uuid policy_uuid,descriptor_version_uuid,body_uuid;
  core::platform::u64 generation=0,body_generation=0,cache_invalidation_mask=0;
  core::catalog::CatalogPolicyDefault decision=core::catalog::CatalogPolicyDefault::deny;
  core::platform::u16 precedence_rank=0;
  core::catalog::CatalogPolicyScope scope=core::catalog::CatalogPolicyScope::database;
  bool body_resolved=false;
};
} // namespace scratchbird::engine::internal_api
