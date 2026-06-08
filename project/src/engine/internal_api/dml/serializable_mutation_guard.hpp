// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DML-SERIALIZABLE-GUARD-ANCHOR
#include "api_types.hpp"

#include <span>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::dml {

struct SerializableDmlAdmissionResult {
  bool ok = true;
  bool active = false;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

SerializableDmlAdmissionResult RecordSerializableSelectRead(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    std::span<const std::string> option_envelopes = {});

SerializableDmlAdmissionResult CheckSerializableInsertMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    std::span<const EngineRowValue> rows,
    std::span<const std::string> option_envelopes = {});

SerializableDmlAdmissionResult RecordSerializableInsertMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    std::span<const EngineRowValue> rows,
    std::span<const std::string> option_envelopes = {});

SerializableDmlAdmissionResult CheckSerializablePredicateMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    bool delete_row,
    std::span<const std::string> option_envelopes = {});

SerializableDmlAdmissionResult RecordSerializablePredicateMutation(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string relation_uuid,
    const EnginePredicateEnvelope& predicate,
    bool delete_row,
    std::span<const std::string> option_envelopes = {});

}  // namespace scratchbird::engine::internal_api::dml
