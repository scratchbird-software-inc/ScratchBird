// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

enum class SblrAssignmentSlotKind {
  local_variable,
  routine_parameter,
  routine_result,
};

struct SblrAssignmentValidationResult {
  bool ok = true;
  std::string diagnostic_id;
  std::string detail;
};

struct SblrAssignmentSlot {
  std::string slot_id;
  std::string descriptor_id;
  std::string domain_descriptor_id;
  SblrAssignmentSlotKind kind = SblrAssignmentSlotKind::local_variable;
  SblrValue value;
  bool assigned = false;
  bool read_only = false;
  bool allow_null = true;
  bool require_domain_validation = false;
};

struct SblrAssignmentFrame {
  std::vector<SblrAssignmentSlot> slots;
};

using SblrAssignmentDomainValidator =
    std::function<SblrAssignmentValidationResult(const SblrAssignmentSlot&, const SblrValue&, const SblrExecutionContext&)>;

SblrResult RegisterSblrAssignmentSlot(std::string_view operation_id,
                                      SblrAssignmentFrame* frame,
                                      SblrAssignmentSlot slot,
                                      const SblrExecutionContext& context);

SblrResult AssignSblrSlot(std::string_view operation_id,
                          SblrAssignmentFrame* frame,
                          std::string_view slot_id,
                          const SblrValue& value,
                          const SblrExecutionContext& context,
                          const SblrAssignmentDomainValidator& domain_validator = {});

SblrResult ReadSblrSlot(std::string_view operation_id,
                        const SblrAssignmentFrame* frame,
                        std::string_view slot_id,
                        const SblrExecutionContext& context);

const char* ToString(SblrAssignmentSlotKind kind);

}  // namespace scratchbird::engine::sblr
