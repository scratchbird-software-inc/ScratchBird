// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class DmlPageAllocationRuntimeFamily {
  row_data,
  index
};

struct DmlPageAllocationRuntimeResult {
  bool active = false;
  std::uint64_t requested_pages = 0;
  std::uint64_t granted_preallocation_pages = 0;
  bool preallocation_requested = false;
  bool preallocation_granted = false;
  bool preallocation_capped = false;
  bool preallocation_refused = false;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;

  bool ok() const {
    return !active || !diagnostic.error;
  }
};

DmlPageAllocationRuntimeResult ReserveDmlPageAllocationRuntime(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const std::string& owner_object_uuid,
    DmlPageAllocationRuntimeFamily family,
    std::uint64_t requested_pages,
    std::string mutation_phase);

DmlPageAllocationRuntimeResult ReserveDmlIndexPageAllocationRuntime(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const CrudState& state,
    const std::string& table_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::string mutation_phase);

DmlPageAllocationRuntimeResult ReserveDmlIndexPageAllocationRuntimeForRows(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const CrudState& state,
    const std::string& table_uuid,
    const std::vector<std::vector<std::pair<std::string, std::string>>>& row_values,
    std::string mutation_phase);

DmlPageAllocationRuntimeResult ReserveDmlIndexPageAllocationRuntimeForRows(
    const EngineRequestContext& context,
    const std::vector<std::string>& option_envelopes,
    const CrudState& state,
    const std::string& table_uuid,
    const std::vector<CrudRowVersionRecord>& rows,
    std::string mutation_phase);

void AddDmlPageAllocationRuntimeEvidence(const DmlPageAllocationRuntimeResult& allocation,
                                         EngineApiResult* result);

}  // namespace scratchbird::engine::internal_api
