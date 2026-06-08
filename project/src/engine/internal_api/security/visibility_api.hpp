// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_VISIBILITY_API
struct EngineEvaluateVisibilityRequest : EngineApiRequest {};
struct EngineEvaluateVisibilityResult : EngineApiResult {};
EngineEvaluateVisibilityResult EngineEvaluateVisibility(const EngineEvaluateVisibilityRequest& request);

// SEARCH_KEY: CLUSTER_PROJECTION_REDACTION
// Local cluster projections are security-filtered views only. They never grant
// cluster truth authority and support-bundle use requires explicit retention
// evidence plus support-export authorization.
enum class ClusterProjectionRedactionSensitivity {
  topology,
  security,
  route,
  metric,
};

struct ClusterProjectionSensitiveField {
  std::string field_name;
  std::string clear_value;
  bool sensitive = true;
};

struct ClusterProjectionRedactedField {
  std::string field_name;
  std::string value;
  bool redacted = true;
  std::string redaction_class;
};

struct EngineEvaluateClusterProjectionRedactionRequest : EngineApiRequest {
  ClusterProjectionRedactionSensitivity sensitivity =
      ClusterProjectionRedactionSensitivity::topology;
  std::string projection_id;
  std::string projection_source;
  std::string target_uuid;
  std::string retention_policy_ref;
  std::string support_bundle_policy_ref;
  bool retention_evidence_present = false;
  bool support_bundle_export = false;
  std::vector<ClusterProjectionSensitiveField> fields;
};

struct EngineEvaluateClusterProjectionRedactionResult : EngineApiResult {
  bool visible = false;
  bool redacted = true;
  bool failed_closed = false;
  bool retention_evidence_present = false;
  bool support_bundle_allowed = false;
  bool local_runtime_execution_enabled = false;
  bool local_projection_cluster_authority = false;
  std::string sensitivity;
  std::string required_right;
  std::string redaction_class;
  std::string retention_policy_ref;
  std::string support_bundle_policy_ref;
  std::vector<ClusterProjectionRedactedField> fields;
};

std::string ClusterProjectionRedactionSensitivityName(
    ClusterProjectionRedactionSensitivity sensitivity);
std::string ClusterProjectionRedactionRequiredRight(
    ClusterProjectionRedactionSensitivity sensitivity);
EngineEvaluateClusterProjectionRedactionResult
EngineEvaluateClusterProjectionRedaction(
    const EngineEvaluateClusterProjectionRedactionRequest& request);

}  // namespace scratchbird::engine::internal_api
