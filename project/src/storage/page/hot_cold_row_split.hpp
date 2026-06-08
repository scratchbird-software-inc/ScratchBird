// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-HOT-COLD-ROW-SPLIT-ANCHOR
#include "large_payload.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

enum class HotColdFieldTemperature {
  hot,
  cold
};

struct HotColdFieldInput {
  std::string field_name;
  std::string encoded_value;
  bool metadata = false;
  bool indexed = false;
  bool frequently_filtered = false;
  bool rare_projection = false;
  bool force_hot = false;
  bool force_cold = false;
};

struct HotColdHotField {
  std::string field_name;
  std::string encoded_value;
  bool metadata = false;
  bool indexed = false;
  bool frequently_filtered = false;
};

struct HotColdColdFieldDescriptor {
  std::string field_name;
  LargePayloadDescriptor descriptor;
  std::string descriptor_text;
  LargePayloadFamily family = LargePayloadFamily::blob;
};

struct HotColdRowHead {
  TypedUuid row_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid transaction_uuid;
  u64 creator_local_transaction_id = 0;
  u64 row_version = 1;
  std::string hot_filespace_class;
  std::string cold_row_filespace_class;
  std::vector<HotColdHotField> hot_fields;
  std::vector<HotColdColdFieldDescriptor> cold_fields;
  std::vector<std::string> evidence;
  bool descriptor_evidence_finality_authority = false;
  bool descriptor_evidence_visibility_authority = false;
};

struct HotColdRowSplitRequest {
  LargePayloadStore* large_payload_store = nullptr;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid owner_object_uuid;
  TypedUuid row_uuid;
  TypedUuid transaction_uuid;
  TypedUuid chunk_policy_uuid;
  u64 local_transaction_id = 0;
  u64 row_version = 1;
  LargePayloadFamily family = LargePayloadFamily::blob;
  u64 cold_threshold_bytes = 4096;
  std::vector<HotColdFieldInput> fields;
  bool engine_storage_admission_authorized = false;
  bool mga_write_admitted_by_transaction_inventory = false;
  std::string reason;
};

struct HotColdRowSplitResult {
  Status status;
  bool split = false;
  HotColdRowHead hot_head;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && split; }
};

struct HotColdRowMaterializeRequest {
  LargePayloadStore* large_payload_store = nullptr;
  HotColdRowHead hot_head;
  std::vector<std::string> cold_field_names;
  u64 observer_snapshot_visible_through_local_transaction_id = 0;
  bool transaction_context_present = false;
  bool engine_storage_admission_authorized = false;
  bool use_cache = true;
  bool prefetch_on_miss = false;
  std::string reason;
};

struct HotColdRowMaterializeResult {
  Status status;
  bool materialized = false;
  std::vector<HotColdHotField> cold_fields;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && materialized; }
};

struct HotColdRowUpdateRequest {
  HotColdRowHead previous_hot_head;
  HotColdRowSplitRequest replacement;
};

struct HotColdRowUpdateResult {
  Status status;
  bool updated = false;
  HotColdRowHead hot_head;
  std::vector<LargePayloadDescriptor> retired_descriptors;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && updated; }
};

const char* HotColdFieldTemperatureName(HotColdFieldTemperature temperature);
std::string SerializeHotColdRowHead(const HotColdRowHead& hot_head);
HotColdRowSplitResult SplitHotColdRow(const HotColdRowSplitRequest& request);
HotColdRowMaterializeResult MaterializeColdFields(const HotColdRowMaterializeRequest& request);
HotColdRowUpdateResult UpdateHotColdRow(const HotColdRowUpdateRequest& request);
DiagnosticRecord MakeHotColdRowSplitDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

}  // namespace scratchbird::storage::page
