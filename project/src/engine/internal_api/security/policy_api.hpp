// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_POLICY_API
struct EngineEvaluatePolicyRequest : EngineApiRequest {};
struct EngineEvaluatePolicyResult : EngineApiResult {};
EngineEvaluatePolicyResult EngineEvaluatePolicy(const EngineEvaluatePolicyRequest& request);

// SEARCH_KEY: POLICY_MUTATION_COMMANDS
// Post-create policy changes are database commands only. Filesystem policy
// packs are create-time seeds and are not runtime mutation authority.
struct EnginePolicyMutationRequest : EngineApiRequest {
  std::string mutation_kind;
  std::string policy_area;
  std::string policy_mode;
  std::string canonical_policy_envelope;
};

struct EnginePolicyMutationResult : EngineApiResult {
  bool mutation_performed = false;
  bool mga_catalog_commit_required = false;
  bool audit_evidence_recorded = false;
  bool generation_invalidated = false;
  bool filesystem_pack_rejected = false;
  EngineApiU64 previous_policy_epoch = 0;
  EngineApiU64 new_policy_epoch = 0;
};

EnginePolicyMutationResult EngineMutatePolicy(const EnginePolicyMutationRequest& request);

}  // namespace scratchbird::engine::internal_api
