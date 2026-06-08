// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "security/visibility_api.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: CLUSTER_SUPPORT_BUNDLE_REDACTION
// SEARCH_KEY: ENTERPRISE_SUPPORT_BUNDLE
// Cluster projection support bundles are redaction-policy outputs only. They
// carry retention evidence and never establish local cluster truth authority.
struct ClusterSupportBundleProjectionSource {
  ClusterProjectionRedactionSensitivity sensitivity =
      ClusterProjectionRedactionSensitivity::topology;
  std::string projection_id;
  std::string projection_source;
  std::string target_uuid;
  std::string retention_policy_ref;
  std::string support_bundle_policy_ref;
  bool retention_evidence_present = false;
  std::vector<ClusterProjectionSensitiveField> fields;
};

struct EngineBuildClusterProjectionSupportBundleRedactionRequest
    : EngineApiRequest {
  std::string support_bundle_id;
  std::string capture_generation;
  std::vector<ClusterSupportBundleProjectionSource> projections;
};

struct EngineBuildClusterProjectionSupportBundleRedactionResult
    : EngineApiResult {
  bool bundle_allowed = false;
  bool sensitive_values_redacted = false;
  bool retention_evidence_present = false;
  bool failed_closed = false;
  bool local_runtime_execution_enabled = false;
  bool local_projection_cluster_authority = false;
  std::string support_bundle_json;
  std::vector<EngineEvaluateClusterProjectionRedactionResult>
      projection_decisions;
};

EngineBuildClusterProjectionSupportBundleRedactionResult
EngineBuildClusterProjectionSupportBundleRedaction(
    const EngineBuildClusterProjectionSupportBundleRedactionRequest& request);

}  // namespace scratchbird::engine::internal_api
