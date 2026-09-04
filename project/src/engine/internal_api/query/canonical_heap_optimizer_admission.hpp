// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "optimizer_catalog_backed_planning.hpp"
#include "query/plan_api.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct PreparedMgaHeapReadAuthorityCohort;

struct CanonicalHeapOptimizerAdmissionRequest {
  EngineRequestContext context;
  TypedRelationalDag relational_dag;
  std::shared_ptr<const PreparedMgaHeapReadAuthorityCohort> authority_cohort;
};

struct CanonicalHeapOptimizerAdmissionIssue {
  std::string diagnostic_id;
  std::string field_id;
};

struct CanonicalHeapOptimizerAdmissionResult {
  bool built{false};
  scratchbird::engine::optimizer::CanonicalOptimizerAdmissionRequest request;
  scratchbird::engine::optimizer::CanonicalOptimizerAdmissionResult admission;
  std::string current_relation_descriptor_uuid;
  std::uint64_t current_relation_descriptor_generation{0};
  std::vector<std::string> current_relation_projection_type_names;
  std::vector<EngineDescriptor> current_relation_projection_descriptors;
  std::shared_ptr<const PreparedMgaHeapReadAuthorityCohort> authority_cohort;
  CanonicalHeapOptimizerAdmissionIssue issue;
};

// QOW-SOURCE-QRY-004-HEAP-OPTIMIZER-ADMISSION-V1
// Builds only pre-access optimizer admission evidence for one current local
// heap relation. The request exposes no caller-supplied catalog, security,
// statistics, planning, execution, or transaction-finality authority.
CanonicalHeapOptimizerAdmissionResult
BuildCanonicalCurrentHeapOptimizerAdmission(
    const CanonicalHeapOptimizerAdmissionRequest& request);

}  // namespace scratchbird::engine::internal_api
