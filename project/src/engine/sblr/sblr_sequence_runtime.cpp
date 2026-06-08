// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_sequence_runtime.hpp"

#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

SblrValue Int64Value(std::string descriptor_id, std::int64_t value) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.payload_kind = SblrValuePayloadKind::signed_integer;
  out.is_null = false;
  out.has_int64_value = true;
  out.int64_value = value;
  out.encoded_value = std::to_string(value);
  out.text_value = out.encoded_value;
  return out;
}

SblrValue TextValue(std::string descriptor_id, std::string value) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.payload_kind = SblrValuePayloadKind::text;
  out.is_null = false;
  out.encoded_value = std::move(value);
  out.text_value = out.encoded_value;
  return out;
}

SblrValue NullValue(std::string descriptor_id) {
  SblrValue out;
  out.descriptor_id = std::move(descriptor_id);
  out.is_null = true;
  return out;
}

SblrResult ScalarResult(std::string operation_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::move(operation_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

SblrRuntimeDiagnostic SequenceDiagnostic(std::string diagnostic_id,
                                         const SblrExecutionContext& context,
                                         std::string sequence_uuid,
                                         std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back({"sequence_uuid", std::move(sequence_uuid)});
  return diagnostic;
}

SblrSequenceState* FindSequenceState(SblrSequenceRegistry* registry, std::string_view sequence_uuid) {
  for (auto& state : registry->states) {
    if (state.definition.sequence_uuid == sequence_uuid) return &state;
  }
  return nullptr;
}

void AppendEvidence(SblrSequenceRegistry* registry,
                    const SblrExecutionContext& context,
                    std::string sequence_uuid,
                    std::string action,
                    std::string value,
                    std::string policy) {
  SblrSequenceEvidenceRecord record;
  record.evidence_sequence = registry->next_evidence_sequence++;
  record.sequence_uuid = std::move(sequence_uuid);
  record.action = std::move(action);
  record.value = std::move(value);
  record.transaction_uuid = context.transaction_uuid;
  record.local_transaction_id = context.local_transaction_id;
  record.policy = std::move(policy);
  registry->evidence.push_back(std::move(record));
}

SblrResult EnsureRegistry(SblrSequenceRegistry* registry,
                          const SblrExecutionContext& context,
                          std::string sequence_uuid,
                          std::string operation_id) {
  if (registry != nullptr) return MakeSblrSuccess(std::move(operation_id));
  return MakeSblrFailure(SblrStatusCode::execution_failed,
                         std::move(operation_id),
                         SequenceDiagnostic("SB_DIAG_SEQUENCE_REGISTRY_REQUIRED",
                                            context,
                                            std::move(sequence_uuid),
                                            "sequence operation requires a sequence registry"));
}

std::optional<std::int64_t> AdvanceValue(const SblrSequenceDefinition& definition,
                                         bool current_present,
                                         std::int64_t current_value,
                                         std::int64_t increment) {
  if (!current_present) return definition.start_value;
  if (increment > 0 && current_value > definition.maximum_value - increment) {
    return definition.cycle ? std::optional<std::int64_t>(definition.minimum_value) : std::nullopt;
  }
  if (increment < 0 && current_value < definition.minimum_value - increment) {
    return definition.cycle ? std::optional<std::int64_t>(definition.maximum_value) : std::nullopt;
  }
  const std::int64_t next = current_value + increment;
  if (next < definition.minimum_value || next > definition.maximum_value) {
    return definition.cycle
               ? std::optional<std::int64_t>(increment >= 0 ? definition.minimum_value : definition.maximum_value)
               : std::nullopt;
  }
  return next;
}

}  // namespace

SblrSequenceRegistry& ProcessSblrSequenceRegistry() {
  static SblrSequenceRegistry registry;
  return registry;
}

SblrResult RegisterSblrSequence(SblrSequenceRegistry* registry,
                                const SblrSequenceDefinition& definition,
                                const SblrExecutionContext& context) {
  auto registry_status = EnsureRegistry(registry, context, definition.sequence_uuid, "sblr.sequence.register");
  if (!registry_status.ok()) return registry_status;
  if (definition.sequence_uuid.empty()) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.register",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_UUID_REQUIRED",
                                              context,
                                              definition.sequence_uuid,
                                              "sequence UUID is required for sequence registration"));
  }
  if (definition.increment == 0) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.register",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_INCREMENT_ZERO",
                                              context,
                                              definition.sequence_uuid,
                                              "sequence increment cannot be zero"));
  }
  if (definition.minimum_value > definition.maximum_value ||
      definition.start_value < definition.minimum_value ||
      definition.start_value > definition.maximum_value) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.register",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_BOUNDS_INVALID",
                                              context,
                                              definition.sequence_uuid,
                                              "sequence start/minimum/maximum bounds are invalid"));
  }
  std::lock_guard<std::mutex> guard(registry->mutex);
  if (auto* existing = FindSequenceState(registry, definition.sequence_uuid)) {
    existing->definition = definition;
    AppendEvidence(registry, context, definition.sequence_uuid, "sequence.rebind", "", "definition_update");
    return MakeSblrSuccess("sblr.sequence.register");
  }
  SblrSequenceState state;
  state.definition = definition;
  registry->states.push_back(std::move(state));
  AppendEvidence(registry, context, definition.sequence_uuid, "sequence.bind", "", "definition_create");
  return MakeSblrSuccess("sblr.sequence.register");
}

SblrResult NextSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request) {
  auto registry_status = EnsureRegistry(registry, request.context, request.sequence_uuid, "sblr.sequence.next");
  if (!registry_status.ok()) return registry_status;
  if (request.sequence_uuid.empty()) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.next",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_UUID_REQUIRED",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence UUID is required for next sequence value"));
  }
  std::lock_guard<std::mutex> guard(registry->mutex);
  SblrSequenceState* state = FindSequenceState(registry, request.sequence_uuid);
  if (state == nullptr) {
    SblrSequenceDefinition definition;
    definition.sequence_uuid = request.sequence_uuid;
    definition.descriptor_id = request.result_descriptor_id.empty() ? "int64" : request.result_descriptor_id;
    registry->states.push_back(SblrSequenceState{definition});
    state = &registry->states.back();
    AppendEvidence(registry, request.context, request.sequence_uuid, "sequence.implicit_bind", "", "planner_test_default");
  }
  const std::int64_t increment = request.has_increment_override ? request.increment_override : state->definition.increment;
  if (increment == 0) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.next",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_INCREMENT_ZERO",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence increment override cannot be zero"));
  }
  if (state->next_value_override_present) {
    const std::int64_t next = state->next_value_override;
    if (next < state->definition.minimum_value || next > state->definition.maximum_value) {
      return MakeSblrFailure(SblrStatusCode::execution_failed,
                             "sblr.sequence.next",
                             SequenceDiagnostic("SB_DIAG_SEQUENCE_BOUNDS_INVALID",
                                                request.context,
                                                request.sequence_uuid,
                                                "sequence next override is outside configured bounds"));
    }
    state->next_value_override_present = false;
    state->current_value = next;
    state->current_value_present = true;
    AppendEvidence(registry,
                   request.context,
                   request.sequence_uuid,
                   "sequence.next",
                   std::to_string(next),
                   "non_transactional_no_rollback");
    const std::string descriptor = request.result_descriptor_id.empty()
                                       ? state->definition.descriptor_id
                                       : request.result_descriptor_id;
    return ScalarResult("sblr.sequence.next", Int64Value(descriptor.empty() ? "int64" : descriptor, next));
  }
  const auto next = AdvanceValue(state->definition, state->current_value_present, state->current_value, increment);
  if (!next.has_value()) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.next",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_EXHAUSTED",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence reached its configured bounds and cycle is disabled"));
  }
  state->current_value = *next;
  state->current_value_present = true;
  AppendEvidence(registry,
                 request.context,
                 request.sequence_uuid,
                 "sequence.next",
                 std::to_string(*next),
                 "non_transactional_no_rollback");
  const std::string descriptor = request.result_descriptor_id.empty()
                                     ? state->definition.descriptor_id
                                     : request.result_descriptor_id;
  return ScalarResult("sblr.sequence.next", Int64Value(descriptor.empty() ? "int64" : descriptor, *next));
}

SblrResult CurrentSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request) {
  auto registry_status = EnsureRegistry(registry, request.context, request.sequence_uuid, "sblr.sequence.current");
  if (!registry_status.ok()) return registry_status;
  std::lock_guard<std::mutex> guard(registry->mutex);
  const auto* state = FindSequenceState(registry, request.sequence_uuid);
  if (state == nullptr || !state->current_value_present) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.current",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_CURRENT_UNDEFINED",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence current value is undefined until NEXT is called in this runtime"));
  }
  AppendEvidence(registry,
                 request.context,
                 request.sequence_uuid,
                 "sequence.current",
                 std::to_string(state->current_value),
                 "read_only");
  const std::string descriptor = request.result_descriptor_id.empty()
                                     ? state->definition.descriptor_id
                                     : request.result_descriptor_id;
  return ScalarResult("sblr.sequence.current", Int64Value(descriptor.empty() ? "int64" : descriptor, state->current_value));
}

SblrResult SetSblrSequenceValue(SblrSequenceRegistry* registry, const SblrSequenceRequest& request) {
  auto registry_status = EnsureRegistry(registry, request.context, request.sequence_uuid, "sblr.sequence.set");
  if (!registry_status.ok()) return registry_status;
  if (request.sequence_uuid.empty()) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.set",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_UUID_REQUIRED",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence UUID is required for set sequence value"));
  }
  std::lock_guard<std::mutex> guard(registry->mutex);
  SblrSequenceState* state = FindSequenceState(registry, request.sequence_uuid);
  if (state == nullptr) {
    SblrSequenceDefinition definition;
    definition.sequence_uuid = request.sequence_uuid;
    definition.descriptor_id = request.result_descriptor_id.empty() ? "int64" : request.result_descriptor_id;
    registry->states.push_back(SblrSequenceState{definition});
    state = &registry->states.back();
    AppendEvidence(registry, request.context, request.sequence_uuid, "sequence.implicit_bind", "", "planner_test_default");
  }
  if (request.set_value < state->definition.minimum_value || request.set_value > state->definition.maximum_value) {
    return MakeSblrFailure(SblrStatusCode::execution_failed,
                           "sblr.sequence.set",
                           SequenceDiagnostic("SB_DIAG_SEQUENCE_BOUNDS_INVALID",
                                              request.context,
                                              request.sequence_uuid,
                                              "sequence set value is outside configured bounds"));
  }
  state->current_value = request.set_value;
  state->current_value_present = true;
  state->next_value_override = request.set_value;
  state->next_value_override_present = !request.is_called;
  AppendEvidence(registry,
                 request.context,
                 request.sequence_uuid,
                 "sequence.set",
                 std::to_string(request.set_value),
                 request.is_called ? "non_transactional_called" : "non_transactional_not_called");
  const std::string descriptor = request.result_descriptor_id.empty()
                                     ? state->definition.descriptor_id
                                     : request.result_descriptor_id;
  return ScalarResult("sblr.sequence.set", Int64Value(descriptor.empty() ? "int64" : descriptor, request.set_value));
}

SblrResult IdentityCurrentValue(const SblrExecutionContext& context) {
  if (!context.last_identity_value_present) {
    return ScalarResult("sblr.identity.current", NullValue("int64"));
  }
  return ScalarResult("sblr.identity.current", TextValue("int64", context.last_identity_value));
}

SblrSequenceOptimizerMetadata SequenceOptimizerMetadata() {
  return SblrSequenceOptimizerMetadata{};
}

SblrResult RefuseSblrSequenceHook(const SblrExecutionContext& context, std::string sequence_uuid) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_SEQUENCE_RUNTIME_REFUSED", context,
                                              "sequence operation was explicitly routed to refusal hook");
  diagnostic.fields.push_back({"sequence_uuid", std::move(sequence_uuid)});
  return MakeSblrFailure(SblrStatusCode::unsupported_feature, "sblr.sequence.next", std::move(diagnostic));
}

}  // namespace scratchbird::engine::sblr
