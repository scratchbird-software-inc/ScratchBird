// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_assignment_runtime.hpp"
#include "sblr_runtime.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

struct SblrDomainRequest {
  SblrExecutionContext context;
  std::string domain_uuid;
  SblrValue value;
  std::string method_name;
};

struct SblrDomainOptimizerMetadata {
  bool descriptor_authoritative = true;
  bool security_sensitive = true;
  bool masking_possible = true;
  bool indexable_when_base_descriptor_indexable = true;
  bool pushdown_allowed = false;
  bool llvm_fold_allowed = false;
  std::string cost_class = "domain_policy";
  std::string selectivity_rule = "domain descriptor policy";
};

SblrResult CastSblrValueToDomain(const SblrDomainRequest& request);
SblrResult CastSblrDomainValueToBase(const SblrDomainRequest& request);
SblrResult ValidateSblrDomainValue(const SblrDomainRequest& request);
SblrResult ApplySblrDomainReadPolicy(const SblrDomainRequest& request);
SblrResult InvokeSblrDomainMethod(const SblrDomainRequest& request);
SblrAssignmentDomainValidator MakeSblrDomainAssignmentValidator();
SblrDomainOptimizerMetadata DomainOptimizerMetadata();
bool LooksLikeSblrDomainDescriptor(std::string_view descriptor_id);

}  // namespace scratchbird::engine::sblr
