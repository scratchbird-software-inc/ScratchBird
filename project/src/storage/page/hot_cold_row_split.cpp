// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hot_cold_row_split.hpp"

#include "filespace_lifecycle.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::storage::page {
namespace {

namespace filespace = scratchbird::storage::filespace;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status HotColdOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status HotColdErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

std::vector<byte> PayloadBytes(const std::string& value) {
  return std::vector<byte>(value.begin(), value.end());
}

std::string Hex(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    out.push_back(kHex[ch >> 4]);
    out.push_back(kHex[ch & 0x0f]);
  }
  return out;
}

bool NameRequested(const std::vector<std::string>& requested, const std::string& field_name) {
  return requested.empty() ||
         std::find(requested.begin(), requested.end(), field_name) != requested.end();
}

bool SamePayloadDescriptor(const LargePayloadDescriptor& left,
                           const LargePayloadDescriptor& right) {
  return left.payload_uuid.valid() &&
         right.payload_uuid.valid() &&
         left.payload_uuid.kind == right.payload_uuid.kind &&
         left.payload_uuid.value == right.payload_uuid.value &&
         left.generation == right.generation;
}

bool ColdFieldExists(const std::vector<HotColdColdFieldDescriptor>& fields,
                     const HotColdColdFieldDescriptor& candidate) {
  for (const auto& field : fields) {
    if (field.field_name == candidate.field_name &&
        SamePayloadDescriptor(field.descriptor, candidate.descriptor)) {
      return true;
    }
  }
  return false;
}

HotColdFieldTemperature ClassifyField(const HotColdFieldInput& field, u64 threshold) {
  if (field.force_cold) {
    return HotColdFieldTemperature::cold;
  }
  if (field.force_hot || field.metadata || field.indexed || field.frequently_filtered) {
    return HotColdFieldTemperature::hot;
  }
  if (field.rare_projection || field.encoded_value.size() > threshold) {
    return HotColdFieldTemperature::cold;
  }
  return HotColdFieldTemperature::hot;
}

HotColdRowSplitResult RefuseSplit(std::string diagnostic_code,
                                  std::string message_key,
                                  std::string detail) {
  HotColdRowSplitResult result;
  result.status = HotColdErrorStatus();
  result.evidence.push_back("hot_cold_split_fail_closed=true");
  result.evidence.push_back("hot_cold_split_refused=" + diagnostic_code);
  result.evidence.push_back("descriptor_finality_authority=false");
  result.evidence.push_back("descriptor_visibility_authority=false");
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

HotColdRowMaterializeResult RefuseMaterialize(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  HotColdRowMaterializeResult result;
  result.status = HotColdErrorStatus();
  result.evidence.push_back("hot_cold_materialize_fail_closed=true");
  result.evidence.push_back("hot_cold_materialize_refused=" + diagnostic_code);
  result.evidence.push_back("descriptor_finality_authority=false");
  result.evidence.push_back("descriptor_visibility_authority=false");
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

HotColdRowUpdateResult RefuseUpdate(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail) {
  HotColdRowUpdateResult result;
  result.status = HotColdErrorStatus();
  result.evidence.push_back("hot_cold_update_fail_closed=true");
  result.evidence.push_back("hot_cold_update_refused=" + diagnostic_code);
  result.evidence.push_back("descriptor_finality_authority=false");
  result.evidence.push_back("descriptor_visibility_authority=false");
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   std::move(diagnostic_code),
                                                   std::move(message_key),
                                                   std::move(detail));
  return result;
}

bool ValidSplitRequest(const HotColdRowSplitRequest& request, std::string* detail) {
  if (request.large_payload_store == nullptr) {
    *detail = "large payload store is required";
    return false;
  }
  if (!request.database_uuid.valid() || !request.filespace_uuid.valid() ||
      !request.owner_object_uuid.valid() || !request.row_uuid.valid() ||
      !request.transaction_uuid.valid()) {
    *detail = "database, filespace, owner object, row, and transaction UUIDs are required";
    return false;
  }
  if (request.local_transaction_id == 0) {
    *detail = "local transaction id is required";
    return false;
  }
  if (!request.engine_storage_admission_authorized ||
      !request.mga_write_admitted_by_transaction_inventory) {
    *detail = "engine storage admission and durable transaction inventory admission are required";
    return false;
  }
  return true;
}

filespace::FilespaceClassDecision ResolveClass(const HotColdRowSplitRequest& request,
                                               filespace::FilespaceObjectClass object_class,
                                               std::string page_family) {
  filespace::FilespaceClassRequest class_request;
  class_request.database_uuid = request.database_uuid;
  class_request.filespace_uuid = request.filespace_uuid;
  class_request.owner_object_uuid = request.owner_object_uuid;
  class_request.object_class = object_class;
  class_request.page_family = std::move(page_family);
  class_request.reason = "hot_cold_row_split";
  class_request.explicit_object_class = true;
  return filespace::ResolveFilespaceClass(class_request);
}

}  // namespace

const char* HotColdFieldTemperatureName(HotColdFieldTemperature temperature) {
  switch (temperature) {
    case HotColdFieldTemperature::hot: return "hot";
    case HotColdFieldTemperature::cold: return "cold";
  }
  return "cold";
}

std::string SerializeHotColdRowHead(const HotColdRowHead& hot_head) {
  std::ostringstream out;
  out << "SB_HOT_COLD_ROW_HEAD_V1"
      << ";row_uuid=" << scratchbird::core::uuid::UuidToString(hot_head.row_uuid.value)
      << ";owner_object_uuid=" << scratchbird::core::uuid::UuidToString(hot_head.owner_object_uuid.value)
      << ";transaction_uuid=" << scratchbird::core::uuid::UuidToString(hot_head.transaction_uuid.value)
      << ";creator_local_tx=" << hot_head.creator_local_transaction_id
      << ";row_version=" << hot_head.row_version
      << ";hot_filespace_class=" << hot_head.hot_filespace_class
      << ";cold_row_filespace_class=" << hot_head.cold_row_filespace_class
      << ";descriptor_finality_authority=false"
      << ";descriptor_visibility_authority=false";
  for (const auto& field : hot_head.hot_fields) {
    out << ";hot_field=" << Hex(field.field_name) << ":" << Hex(field.encoded_value);
  }
  for (const auto& field : hot_head.cold_fields) {
    out << ";cold_field=" << Hex(field.field_name) << ":" << Hex(field.descriptor_text);
  }
  return out.str();
}

HotColdRowSplitResult SplitHotColdRow(const HotColdRowSplitRequest& request) {
  std::string invalid_detail;
  if (!ValidSplitRequest(request, &invalid_detail)) {
    return RefuseSplit("hot_cold_split_admission_refused",
                       "storage.page.hot_cold_split.admission_refused",
                       invalid_detail);
  }
  const auto hot_class = ResolveClass(request, filespace::FilespaceObjectClass::hot_row, "data");
  if (!hot_class.ok() || hot_class.filespace_class != filespace::FilespaceClass::hot_row) {
    return RefuseSplit("hot_cold_split_hot_filespace_refused",
                       "storage.page.hot_cold_split.hot_filespace_refused",
                       hot_class.diagnostic.diagnostic_code);
  }
  const auto cold_class = ResolveClass(request, filespace::FilespaceObjectClass::cold_row, "data");
  if (!cold_class.ok() || cold_class.filespace_class != filespace::FilespaceClass::cold_row) {
    return RefuseSplit("hot_cold_split_cold_filespace_refused",
                       "storage.page.hot_cold_split.cold_filespace_refused",
                       cold_class.diagnostic.diagnostic_code);
  }

  HotColdRowSplitResult result;
  result.status = HotColdOkStatus();
  result.split = true;
  result.hot_head.row_uuid = request.row_uuid;
  result.hot_head.owner_object_uuid = request.owner_object_uuid;
  result.hot_head.transaction_uuid = request.transaction_uuid;
  result.hot_head.creator_local_transaction_id = request.local_transaction_id;
  result.hot_head.row_version = request.row_version == 0 ? 1 : request.row_version;
  result.hot_head.hot_filespace_class = filespace::FilespaceClassName(hot_class.filespace_class);
  result.hot_head.cold_row_filespace_class = filespace::FilespaceClassName(cold_class.filespace_class);
  result.hot_head.descriptor_evidence_finality_authority = false;
  result.hot_head.descriptor_evidence_visibility_authority = false;

  for (const auto& field : request.fields) {
    const auto temperature = ClassifyField(field, request.cold_threshold_bytes);
    result.evidence.push_back("field:" + field.field_name + "=" +
                              HotColdFieldTemperatureName(temperature));
    if (temperature == HotColdFieldTemperature::hot) {
      result.hot_head.hot_fields.push_back({field.field_name,
                                            field.encoded_value,
                                            field.metadata,
                                            field.indexed,
                                            field.frequently_filtered});
      continue;
    }

    LargePayloadStoreRequest storage_request;
    storage_request.database_uuid = request.database_uuid;
    storage_request.filespace_uuid = request.filespace_uuid;
    storage_request.owner_object_uuid = request.owner_object_uuid;
    storage_request.generation_scope_uuid = request.row_uuid;
    storage_request.transaction_uuid = request.transaction_uuid;
    storage_request.chunk_policy_uuid = request.chunk_policy_uuid.valid()
                                            ? request.chunk_policy_uuid
                                            : request.owner_object_uuid;
    storage_request.local_transaction_id = request.local_transaction_id;
    storage_request.family = request.family;
    storage_request.payload_bytes = PayloadBytes(field.encoded_value);
    storage_request.inline_threshold_bytes = 0;
    storage_request.allow_inline_payload = false;
    storage_request.retire_previous_generations = false;
    storage_request.reason =
        "hot_cold_row_split;diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
    storage_request.mga_write_admitted_by_transaction_inventory =
        request.mga_write_admitted_by_transaction_inventory;
    auto stored = StoreLargePayloadGeneration(request.large_payload_store, storage_request);
    if (!stored.ok()) {
      return RefuseSplit("hot_cold_split_cold_payload_refused",
                         "storage.page.hot_cold_split.cold_payload_refused",
                         stored.diagnostic.diagnostic_code);
    }
    HotColdColdFieldDescriptor cold;
    cold.field_name = field.field_name;
    cold.descriptor = stored.descriptor;
    cold.descriptor_text = SerializeLargePayloadDescriptor(stored.descriptor);
    cold.family = request.family;
    result.hot_head.cold_fields.push_back(std::move(cold));
    result.evidence.push_back("cold_descriptor:" + field.field_name + ":generation=" +
                              std::to_string(stored.descriptor.generation));
    result.evidence.push_back("cold_descriptor:" + field.field_name + ":filespace_class=" +
                              stored.descriptor.filespace_class);
  }

  result.hot_head.evidence = result.evidence;
  result.hot_head.evidence.push_back("hot_filespace_class=" + result.hot_head.hot_filespace_class);
  result.hot_head.evidence.push_back("cold_row_filespace_class=" + result.hot_head.cold_row_filespace_class);
  result.hot_head.evidence.push_back("descriptor_finality_authority=false");
  result.hot_head.evidence.push_back("descriptor_visibility_authority=false");
  result.hot_head.evidence.push_back("mga_visibility_authority=durable_transaction_inventory");
  result.evidence = result.hot_head.evidence;
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   "ok",
                                                   "storage.page.hot_cold_split.split",
                                                   "hot row head contains only selected hot fields and cold descriptors");
  return result;
}

HotColdRowMaterializeResult MaterializeColdFields(const HotColdRowMaterializeRequest& request) {
  if (request.large_payload_store == nullptr) {
    return RefuseMaterialize("hot_cold_materialize_missing_store",
                             "storage.page.hot_cold_split.materialize_missing_store",
                             "large payload store is required");
  }
  if (!request.transaction_context_present ||
      !request.engine_storage_admission_authorized ||
      request.observer_snapshot_visible_through_local_transaction_id == 0) {
    return RefuseMaterialize("hot_cold_materialize_visibility_context_refused",
                             "storage.page.hot_cold_split.materialize_visibility_context_refused",
                             "transaction context, engine admission, and MGA snapshot are required");
  }

  HotColdRowMaterializeResult result;
  result.status = HotColdOkStatus();
  result.materialized = true;
  for (const auto& field : request.hot_head.cold_fields) {
    if (!NameRequested(request.cold_field_names, field.field_name)) {
      continue;
    }
    LargePayloadReadRequest read;
    read.descriptor = field.descriptor;
    read.observer_snapshot_visible_through_local_transaction_id =
        request.observer_snapshot_visible_through_local_transaction_id;
    read.use_cache = request.use_cache;
    read.prefetch_on_miss = request.prefetch_on_miss;
    read.reason = request.reason.empty()
                      ? "hot_cold_materialize;cache_evidence_not_visibility_authority"
                      : request.reason;
    auto loaded = ReadLargePayloadGeneration(request.large_payload_store, read);
    if (!loaded.ok()) {
      return RefuseMaterialize("hot_cold_materialize_cold_payload_refused",
                               "storage.page.hot_cold_split.materialize_cold_payload_refused",
                               loaded.diagnostic.diagnostic_code);
    }
    result.cold_fields.push_back({field.field_name,
                                  std::string(loaded.payload_bytes.begin(), loaded.payload_bytes.end()),
                                  false,
                                  false,
                                  false});
    result.evidence.push_back("materialized:" + field.field_name);
    result.evidence.push_back(std::string("cache_hit:") + field.field_name + "=" +
                              (loaded.cache_hit ? "true" : "false"));
  }
  result.evidence.push_back("descriptor_finality_authority=false");
  result.evidence.push_back("descriptor_visibility_authority=false");
  result.evidence.push_back("cache_prefetch_evidence_authority=diagnostic_only");
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   "ok",
                                                   "storage.page.hot_cold_split.materialized",
                                                   "cold fields materialized after MGA visibility check");
  return result;
}

HotColdRowUpdateResult UpdateHotColdRow(const HotColdRowUpdateRequest& request) {
  auto split = SplitHotColdRow(request.replacement);
  if (!split.ok()) {
    return RefuseUpdate("hot_cold_update_replacement_refused",
                        "storage.page.hot_cold_split.update_replacement_refused",
                        split.diagnostic.diagnostic_code);
  }

  HotColdRowUpdateResult result;
  result.status = HotColdOkStatus();
  result.updated = true;
  result.hot_head = std::move(split.hot_head);
  result.evidence = split.evidence;

  for (const auto& previous : request.previous_hot_head.cold_fields) {
    if (ColdFieldExists(result.hot_head.cold_fields, previous)) {
      continue;
    }
    LargePayloadRetireRequest retire;
    retire.descriptor = previous.descriptor;
    retire.retiring_local_transaction_id = request.replacement.local_transaction_id;
    retire.mga_write_admitted_by_transaction_inventory =
        request.replacement.mga_write_admitted_by_transaction_inventory;
    retire.reason =
        "hot_cold_row_update;diagnostic_only=true;finality_authority=false;visibility_authority=false;mga_authority=durable_transaction_inventory";
    auto retired = RetireLargePayloadGeneration(request.replacement.large_payload_store, retire);
    if (!retired.ok()) {
      return RefuseUpdate("hot_cold_update_retire_refused",
                          "storage.page.hot_cold_split.update_retire_refused",
                          retired.diagnostic.diagnostic_code);
    }
    result.retired_descriptors.push_back(retired.descriptor);
    result.evidence.push_back("retired_cold_descriptor:" + previous.field_name +
                              ":generation=" + std::to_string(previous.descriptor.generation));
  }
  result.evidence.push_back("descriptor_finality_authority=false");
  result.evidence.push_back("descriptor_visibility_authority=false");
  result.diagnostic = MakeHotColdRowSplitDiagnostic(result.status,
                                                   "ok",
                                                   "storage.page.hot_cold_split.updated",
                                                   "old cold descriptors retired by MGA transaction");
  return result;
}

DiagnosticRecord MakeHotColdRowSplitDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.hot_cold_row_split",
                        status.ok() ? "" : "fail closed and retry after engine MGA authority is available");
}

}  // namespace scratchbird::storage::page
