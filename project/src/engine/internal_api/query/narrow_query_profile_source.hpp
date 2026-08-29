// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Exact engine-private execution source for the three admitted SBQNPB01
// profiles.  The source can only be opened with the move-only authority
// returned by ConsumeNarrowQueryBindingAuthorityV1.  It does not accept SQL,
// names, parser demands, raw relation UUIDs, or a generic-plan fallback.

#include "query/narrow_query_binding_authority.hpp"
#include "query/narrow_query_typed_result_publication.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

inline constexpr std::uint16_t kNarrowQueryProfileSourceVersionV1 = 1;

enum class EngineNarrowQueryProfileSourceStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  access_denied,
  profile_unavailable,
  binding_stale,
  descriptor_invalid,
  datatype_invalid,
  collation_invalid,
  resource_budget_exceeded,
  cancelled,
  storage_refused,
};

struct EngineNarrowQueryProfileSourceOpenRequestV1 {
  std::uint16_t version = kNarrowQueryProfileSourceVersionV1;
  EngineRequestContext context;
  EngineNarrowQueryBindingAuthorityHandleV1 binding_authority;
};

struct EngineNarrowQueryProfileSourceOpenResultV1 {
  EngineNarrowQueryProfileSourceStatusV1 status =
      EngineNarrowQueryProfileSourceStatusV1::invalid_argument;
  EngineApiDiagnostic diagnostic;
  std::unique_ptr<NarrowQueryTypedResultOccurrenceSourceV1> source;

  [[nodiscard]] bool ok() const {
    return status == EngineNarrowQueryProfileSourceStatusV1::ok &&
           source != nullptr;
  }

  // A refusal on an exact Core profile is terminal.  The caller may not
  // reinterpret it as a generic query request.
  [[nodiscard]] constexpr bool generic_fallback_permitted() const {
    return false;
  }
};

struct EngineNarrowQueryProfileSourceOccurrenceMetricsV1 {
  std::uint32_t source_ordinal = 0;
  std::uint64_t visible_row_count = 0;
  std::uint64_t delivered_row_count = 0;
  std::uint64_t second_pass_scanned_row_version_count = 0;
  std::uint64_t second_pass_decoded_byte_count = 0;
  std::uint64_t second_pass_storage_bytes_read = 0;
  bool complete_value_delivery = false;
  bool delivery_stopped_by_bound = false;
};

struct EngineNarrowQueryProfileSourceExecutionMetricsV1 {
  std::vector<EngineNarrowQueryProfileSourceOccurrenceMetricsV1> sources;
};

EngineNarrowQueryProfileSourceOpenResultV1 OpenNarrowQueryProfileSourceV1(
    EngineNarrowQueryProfileSourceOpenRequestV1 request);

// Engine-private, immutable terminal observation.  Metrics are available only
// after successful EOS and before Close releases the retained binding
// authority.  The pointer must identify this exact narrow-profile source type;
// generic or wrapped typed-result sources are intentionally non-observable.
bool InspectNarrowQueryProfileSourceExecutionMetricsV1(
    const NarrowQueryTypedResultOccurrenceSourceV1* source,
    EngineNarrowQueryProfileSourceExecutionMetricsV1* metrics);

const char* EngineNarrowQueryProfileSourceStatusNameV1(
    EngineNarrowQueryProfileSourceStatusV1 status);

}  // namespace scratchbird::engine::internal_api
