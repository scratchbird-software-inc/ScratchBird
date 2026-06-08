// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_assignment_runtime.hpp"

#include "sblr_special_forms.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrResult AssignmentFailure(std::string_view operation_id,
                             const SblrExecutionContext& context,
                             std::string diagnostic_id,
                             std::string detail,
                             std::string_view slot_id = {}) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  if (!slot_id.empty()) diagnostic.fields.push_back({"slot_id", std::string(slot_id)});
  diagnostic.fields.push_back({"operation_id", std::string(operation_id)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(operation_id), std::move(diagnostic));
}

SblrAssignmentSlot* FindSlot(SblrAssignmentFrame* frame, std::string_view slot_id) {
  if (frame == nullptr) return nullptr;
  const auto it = std::find_if(frame->slots.begin(), frame->slots.end(), [slot_id](const SblrAssignmentSlot& slot) {
    return slot.slot_id == slot_id;
  });
  return it == frame->slots.end() ? nullptr : &*it;
}

const SblrAssignmentSlot* FindSlot(const SblrAssignmentFrame* frame, std::string_view slot_id) {
  if (frame == nullptr) return nullptr;
  const auto it = std::find_if(frame->slots.begin(), frame->slots.end(), [slot_id](const SblrAssignmentSlot& slot) {
    return slot.slot_id == slot_id;
  });
  return it == frame->slots.end() ? nullptr : &*it;
}

SblrResult SlotScalarResult(std::string_view operation_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::string(operation_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

}  // namespace

const char* ToString(SblrAssignmentSlotKind kind) {
  switch (kind) {
    case SblrAssignmentSlotKind::local_variable:
      return "local_variable";
    case SblrAssignmentSlotKind::routine_parameter:
      return "routine_parameter";
    case SblrAssignmentSlotKind::routine_result:
      return "routine_result";
  }
  return "unknown";
}

SblrResult RegisterSblrAssignmentSlot(std::string_view operation_id,
                                      SblrAssignmentFrame* frame,
                                      SblrAssignmentSlot slot,
                                      const SblrExecutionContext& context) {
  if (frame == nullptr) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_FRAME_REQUIRED", "assignment frame is required");
  }
  if (slot.slot_id.empty()) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_ID_REQUIRED", "assignment slot id is required");
  }
  if (FindSlot(frame, slot.slot_id) != nullptr) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_DUPLICATE", "assignment slot id is already registered", slot.slot_id);
  }
  if (slot.value.is_null && !slot.descriptor_id.empty()) {
    slot.value.descriptor_id = slot.descriptor_id;
  }
  slot.require_domain_validation = slot.require_domain_validation || !slot.domain_descriptor_id.empty();
  frame->slots.push_back(std::move(slot));
  return MakeSblrSuccess(std::string(operation_id));
}

SblrResult AssignSblrSlot(std::string_view operation_id,
                          SblrAssignmentFrame* frame,
                          std::string_view slot_id,
                          const SblrValue& value,
                          const SblrExecutionContext& context,
                          const SblrAssignmentDomainValidator& domain_validator) {
  SblrAssignmentSlot* slot = FindSlot(frame, slot_id);
  if (slot == nullptr) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_NOT_FOUND", "assignment slot is not registered", slot_id);
  }
  if (slot->read_only) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_READ_ONLY", "assignment slot is read-only", slot_id);
  }
  if (value.is_null && !slot->allow_null) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_NULL_FORBIDDEN", "assignment slot does not allow null", slot_id);
  }

  SblrValue assigned = value;
  if (!slot->descriptor_id.empty()) {
    const auto cast = EvaluateSblrCastForm(operation_id, value, slot->descriptor_id, context, true, false);
    if (!cast.ok()) return cast;
    if (!cast.scalar_values.empty()) assigned = cast.scalar_values.front();
    if (assigned.is_null) assigned.descriptor_id = slot->descriptor_id;
  }

  if (slot->require_domain_validation) {
    if (!domain_validator) {
      return AssignmentFailure(operation_id,
                               context,
                               "SBLR.ASSIGNMENT_DOMAIN_VALIDATOR_REQUIRED",
                               "assignment requires domain validation but no domain validator was supplied",
                               slot_id);
    }
    const auto validation = domain_validator(*slot, assigned, context);
    if (!validation.ok) {
      return AssignmentFailure(operation_id,
                               context,
                               validation.diagnostic_id.empty() ? "SBLR.ASSIGNMENT_DOMAIN_VALIDATION_FAILED" : validation.diagnostic_id,
                               validation.detail.empty() ? "domain validation rejected assigned value" : validation.detail,
                               slot_id);
    }
  }

  slot->value = std::move(assigned);
  slot->assigned = true;
  return SlotScalarResult(operation_id, slot->value);
}

SblrResult ReadSblrSlot(std::string_view operation_id,
                        const SblrAssignmentFrame* frame,
                        std::string_view slot_id,
                        const SblrExecutionContext& context) {
  const SblrAssignmentSlot* slot = FindSlot(frame, slot_id);
  if (slot == nullptr) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_NOT_FOUND", "assignment slot is not registered", slot_id);
  }
  if (!slot->assigned) {
    return AssignmentFailure(operation_id, context, "SBLR.ASSIGNMENT_SLOT_UNASSIGNED", "assignment slot has not been assigned", slot_id);
  }
  return SlotScalarResult(operation_id, slot->value);
}

}  // namespace scratchbird::engine::sblr
