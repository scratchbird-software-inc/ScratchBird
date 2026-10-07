// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_record_codec.hpp"
#include "catalog_metric_current_value.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_table_definition.hpp"
#include "catalog_security_record_codec.hpp"
#include "catalog_storage_record_codec.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "catalog_storage_action_policy.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "catalog_scheduler_policy.hpp"
#include "catalog_scheduler_fairness.hpp"
#include "catalog_metric_visibility_policy.hpp"
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_label_schema.hpp"
#include "catalog_metric_series.hpp"

#include "uuid.hpp"
#include "hash_digest.hpp"
#include "hash_digest_parts.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
constexpr std::size_t kBinaryHeaderBytes = 96;
constexpr std::size_t kMaxBinaryRecordBytes = 131072;
using scratchbird::storage::page::CatalogPageRowKind;

Status CodecOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CodecErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

CatalogTypedRecordViewResult RecordViewError(std::string_view code,
    std::string_view key, std::string_view detail = {},
    std::string_view origin = "core.catalog.record_codec") {
  return {{},{CodecErrorStatus(),code,key,detail,origin}};
}
CatalogRecordCodecResult MaterializeRecordError(const CatalogRecordDiagnosticView& error) {
  CatalogRecordCodecResult result;result.status=error.status;
  result.diagnostic=MaterializeCatalogRecordDiagnostic(error);return result;
}
std::optional<CatalogRecordDiagnosticView> DurableIdentityError(const TypedUuid& id) {
  const auto error=[&](const char* code,const char* key,bool detail=true) {
    return CatalogRecordDiagnosticView{
        {StatusCode::uuid_invalid,Severity::error,Subsystem::uuid},code,key,
        detail?std::string_view(scratchbird::core::uuid::UuidKindName(id.kind)):std::string_view{}, "core.uuid"};
  };
  using namespace scratchbird::core::uuid;
  if(!UuidKindAllowsDurableIdentity(id.kind))
    return error("SB-UUID-DURABLE-IDENTITY-KIND","uuid.durable_identity.kind_not_allowed");
  if(!IsEngineIdentityKind(id.kind))return error("SB-UUID-TYPED-UNKNOWN-KIND","uuid.typed.unknown_kind",false);
  if(id.value.is_nil())return error("SB-UUID-TYPED-NIL","uuid.typed.nil_not_allowed");
  if(!IsValidUuidVariant(id.value))return error("SB-UUID-TYPED-VARIANT","uuid.typed.invalid_variant");
  if(UuidVersion(id.value)!=7)return error("SB-UUID-TYPED-ENGINE-IDENTITY-NOT-V7","uuid.typed.engine_identity_requires_v7");
  return {};
}

bool IsTypedIdentity(const scratchbird::core::platform::TypedUuid& uuid, UuidKind expected) {
  return uuid.kind == expected && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsSuppliedIdentity(const scratchbird::core::platform::TypedUuid& uuid) {
  // Only the default pair denotes absence. A malformed supplied reference
  // must not vanish behind TypedUuid::valid() during catalog serialization.
  return uuid.kind != UuidKind::unknown || !uuid.value.is_nil();
}

}  // namespace

CatalogTypedRecordViewResult ValidateCatalogTypedRecordView(CatalogTypedRecordView record) {
  const auto* descriptor=FindBuiltinCatalogRecordDescriptor(record.header.kind);
  if(!descriptor)return RecordViewError("SB-CATALOG-RECORD-UNKNOWN-KIND",
      "catalog.record.unknown_kind",CatalogRecordKindName(record.header.kind),"core.catalog.records");
  if (record.header.record_version < kCatalogRecordSchemaVersionMinSupported ||
      record.header.record_version > kCatalogRecordSchemaVersionMaxSupported) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                      "catalog.record_codec.version_unsupported",
                      CatalogRecordKindName(record.header.kind));
  }
  if ((descriptor->requires_row_uuid || IsSuppliedIdentity(record.header.row_uuid)) &&
      !IsTypedIdentity(record.header.row_uuid, UuidKind::row)) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-ROW-UUID-MUST-BE-V7",
                      "catalog.record_codec.row_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if ((descriptor->requires_object_uuid || IsSuppliedIdentity(record.header.object_uuid)) &&
      !IsTypedIdentity(record.header.object_uuid, UuidKind::object)) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-OBJECT-UUID-MUST-BE-V7",
                      "catalog.record_codec.object_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor->requires_parent_uuid && record.header.parent_uuid.valid() &&
      !IsEngineIdentityUuid(record.header.parent_uuid.value)) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-PARENT-UUID-MUST-BE-V7",
                      "catalog.record_codec.parent_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor->requires_parent_uuid && !record.header.parent_uuid.valid()) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-PARENT-UUID-REQUIRED",
                      "catalog.record_codec.parent_uuid_required",
                      CatalogRecordKindName(record.header.kind));
  }

  if (IsSuppliedIdentity(record.header.parent_uuid)) {
    if(const auto error=DurableIdentityError(record.header.parent_uuid))return {{},*error};
  }

  // These are ordinary-table family schemas, not a transition of every record
  // sharing the broad table/column kinds (including bootstrap descriptors).
  // Complete metadata admission also dispatches on the explicit subtype, so a
  // corrupt schema marker cannot bypass an ordinary-table family check.
  if (IsCatalogTableDefinitionPayload(record.payload) &&
      !CatalogTableDefinitionMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.table_definition.invalid",
                      "table_binary_payload_or_header_invalid");
  if (IsCatalogColumnDefinitionPayload(record.payload) &&
      !CatalogColumnDefinitionMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.column_definition.invalid",
                      "column_binary_payload_or_header_invalid");
  if (IsCatalogRuntimeAuthorityBindingPayload(record.payload) &&
      !CatalogRuntimeAuthorityBindingMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.runtime_authority_binding.invalid",
                      "runtime_authority_binary_payload_or_header_invalid");
  if (record.header.kind == CatalogRecordKind::storage_descriptor &&
      !CatalogStoragePayloadMatchesHeader(record)) {
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.storage_record.invalid",
                      "storage_binary_payload_or_header_invalid");
  }
  if (IsCatalogSecurityRecordKind(record.header.kind) &&
      !CatalogSecurityPayloadMatchesHeader(record)) {
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.security_record.invalid",
                      "security_binary_payload_or_header_invalid");
  }
  if ((record.header.kind == CatalogRecordKind::metric_current_value ||
       IsCatalogMetricCurrentValuePayload(record.payload)) &&
      !CatalogMetricCurrentValueMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_current_value.invalid",
                      "current_value_binary_payload_or_header_invalid");
  if ((record.header.kind == CatalogRecordKind::metric_series || IsCatalogMetricSeriesPayload(record.payload)) &&
      !CatalogMetricSeriesMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_series.invalid",
                      "series_binary_payload_or_header_invalid");
  if ((record.header.kind == CatalogRecordKind::metric_label_schema || IsCatalogMetricLabelSchemaPayload(record.payload)) &&
      !CatalogMetricLabelSchemaMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_label_schema.invalid",
                      "label_schema_binary_payload_or_header_invalid");
  if ((record.header.kind == CatalogRecordKind::metric_descriptor || IsCatalogMetricDescriptorPayload(record.payload)) &&
      !CatalogMetricDescriptorMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_descriptor.invalid",
                      "descriptor_binary_payload_or_header_invalid");
  if (IsCatalogMetricRetentionPolicyPayload(record.payload) &&
      !CatalogMetricRetentionPolicyMatchesHeader(record)) {
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_retention.invalid",
                      "policy_binary_payload_or_header_invalid");
  }
  if (IsCatalogStorageActionAttachmentPayload(record.payload) &&
      !CatalogStorageActionAttachmentMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.storage_action.invalid",
                      "storage_action_attachment_binary_payload_or_header_invalid");
  if (IsCatalogStorageActionPolicyPayload(record.payload) &&
      !CatalogStorageActionPolicyMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.storage_action.invalid",
                      "storage_action_binary_payload_or_header_invalid");
  if (IsCatalogMetricVisibilityPolicyPayload(record.payload) &&
      !CatalogMetricVisibilityPolicyMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.metric_visibility.invalid",
                      "visibility_binary_payload_or_header_invalid");
  if (IsCatalogSchedulerPolicyPayload(record.payload) &&
      !CatalogSchedulerPolicyMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.scheduler_runtime.invalid",
                      "scheduler_runtime_binary_payload_or_header_invalid");
  if (IsCatalogSchedulerQueueProfilePayload(record.payload) &&
      !CatalogSchedulerQueueProfileMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.scheduler_queues.invalid",
                      "scheduler_queues_binary_payload_or_header_invalid");
  if (IsCatalogSchedulerFairnessProfilePayload(record.payload) &&
      !CatalogSchedulerFairnessProfileMatchesHeader(record))
    return RecordViewError("CATALOG.INVALID_INPUT", "catalog.scheduler_fairness.invalid",
                      "scheduler_fairness_binary_payload_or_header_invalid");
  if (record.payload.size() > kMaxBinaryRecordBytes - kBinaryHeaderBytes) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                      "catalog.record_codec.fields_missing", "binary_record_size_limit");
  }
  return {record,{}};
}

namespace {
// Caller supplied the exact zero-initialized final extent and validated record.
void WriteTypedRecord(CatalogTypedRecordView record, byte* bytes) {
  std::memcpy(bytes, "SBCTREC2", 8);
  StoreLittle16(bytes + 8, 2);
  StoreLittle16(bytes + 10, kBinaryHeaderBytes);
  StoreLittle32(bytes + 12, static_cast<u32>(kBinaryHeaderBytes + record.payload.size()));
  StoreLittle16(bytes + 16, static_cast<u16>(record.header.kind));
  StoreLittle32(bytes + 20, record.header.record_version);
  StoreLittle32(bytes + 24, record.header.deleted ? 1 : 0);
  StoreLittle32(bytes + 28, static_cast<u32>(record.payload.size()));
  const scratchbird::core::platform::TypedUuid* identities[] = {
      &record.header.row_uuid, &record.header.object_uuid, &record.header.parent_uuid};
  for (std::size_t i = 0; i < 3; ++i) {
    bytes[32 + i] = static_cast<byte>(identities[i]->kind);
    const auto& id = identities[i]->value.bytes;
    std::copy(id.begin(), id.end(), bytes + 40 + i * 16);
  }
  std::copy(record.payload.begin(), record.payload.end(),
            bytes + kBinaryHeaderBytes);
}
}

CatalogRecordCodecResult EncodeCatalogTypedRecord(const CatalogTypedRecord& record, u32 ordinal) {
  const auto checked=ValidateCatalogTypedRecordView(BorrowCatalogTypedRecord(record));
  if(!checked.ok())return MaterializeRecordError(checked.diagnostic);
  CatalogRecordCodecResult result;
  result.status = CodecOkStatus();
  result.record = record;
  result.row.kind = CatalogPageRowKind::typed_catalog_record;
  result.row.ordinal = ordinal;
  result.row.payload.assign(kBinaryHeaderBytes + record.payload.size(), '\0');
  WriteTypedRecord(BorrowCatalogTypedRecord(record), reinterpret_cast<byte*>(result.row.payload.data()));
  return result;
}

CatalogTypedRecordViewResult DecodeCatalogTypedRecordView(CatalogPageRowKind kind, std::string_view payload) {
  if (kind != CatalogPageRowKind::typed_catalog_record) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-ROW-KIND-INVALID",
                      "catalog.record_codec.row_kind_invalid");
  }
  const auto malformed = [](const char* detail) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                      "catalog.record_codec.fields_missing", detail);
  };
  if (payload.size() < kBinaryHeaderBytes || payload.size() > kMaxBinaryRecordBytes) {
    return malformed("binary_record_size_invalid");
  }
  const auto* bytes = reinterpret_cast<const byte*>(payload.data());
  if (std::memcmp(bytes, "SBCTREC2", 8) != 0 || LoadLittle16(bytes + 8) != 2 ||
      LoadLittle16(bytes + 10) != kBinaryHeaderBytes) {
    return RecordViewError("SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                      "catalog.record_codec.version_unsupported", "binary_header_required");
  }
  if (LoadLittle32(bytes + 12) != payload.size() ||
      LoadLittle32(bytes + 28) != payload.size() - kBinaryHeaderBytes ||
      LoadLittle16(bytes + 18) != 0 || (LoadLittle32(bytes + 24) & ~1u) != 0 ||
      !std::all_of(bytes + 35, bytes + 40, [](byte value) { return value == 0; }) ||
      !std::all_of(bytes + 88, bytes + 96, [](byte value) { return value == 0; })) {
    return malformed("binary_lengths_flags_or_reserved_invalid");
  }
  CatalogTypedRecordView record;
  record.header.kind = static_cast<CatalogRecordKind>(LoadLittle16(bytes + 16));
  record.header.record_version = LoadLittle32(bytes + 20);
  record.header.deleted = LoadLittle32(bytes + 24) != 0;
  scratchbird::core::platform::TypedUuid* identities[] = {
      &record.header.row_uuid, &record.header.object_uuid, &record.header.parent_uuid};
  for (std::size_t i = 0; i < 3; ++i) {
    identities[i]->kind = static_cast<UuidKind>(bytes[32 + i]);
    std::copy(bytes + 40 + i * 16, bytes + 56 + i * 16, identities[i]->value.bytes.begin());
  }
  record.payload=payload.substr(kBinaryHeaderBytes);
  return ValidateCatalogTypedRecordView(record);
}

CatalogRecordCodecResult DecodeCatalogTypedRecord(const CatalogPageRow& row) {
  const auto decoded=DecodeCatalogTypedRecordView(row.kind,row.payload);
  if(!decoded.ok())return MaterializeRecordError(decoded.diagnostic);
  CatalogRecordCodecResult result;result.status=CodecOkStatus();
  result.record={decoded.record->header,std::string(decoded.record->payload)};
  result.row=row;
  return result;
}

DiagnosticRecord MaterializeCatalogRecordDiagnostic(const CatalogRecordDiagnosticView& error) {
  std::vector<DiagnosticArgument> arguments;
  if(!error.detail.empty())arguments.push_back({"detail",std::string(error.detail)});
  return MakeDiagnostic(error.status.code,error.status.severity,error.status.subsystem,
      std::string(error.diagnostic_code),std::string(error.message_key),std::move(arguments),{},std::string(error.origin));
}

namespace {
constexpr std::size_t kMetadataHeaderBytes = 384;
constexpr std::size_t kMetadataMaxBytes = 262144;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle64;
using Metadata = CatalogMetadataVersionView;
constexpr auto kMetadataReferences = std::array{
    &Metadata::owner_uuid, &Metadata::creator_transaction_uuid,
    &Metadata::retired_transaction_uuid, &Metadata::default_name_uuid,
    &Metadata::name_vector_uuid, &Metadata::security_policy_uuid,
    &Metadata::dependency_group_uuid, &Metadata::storage_binding_uuid,
    &Metadata::donor_overlay_uuid, &Metadata::audit_uuid, &Metadata::owning_schema_uuid};
constexpr auto kMetadataCounters = std::array{
    &Metadata::definition_version, &Metadata::schema_epoch, &Metadata::security_epoch,
    &Metadata::resource_epoch, &Metadata::catalog_generation,
    &Metadata::dependency_generation, &Metadata::invalidation_generation,
    &Metadata::creator_local_transaction_id};

CatalogRecordDiagnosticView MetadataViewError(const char* detail) {
  return {CodecErrorStatus(),"CATALOG.INVALID_INPUT","catalog.metadata_version.invalid",
          detail,"core.catalog.record_codec"};
}
CatalogMetadataVersionCodecResult MetadataFailure(const CatalogRecordDiagnosticView& error) {
  CatalogMetadataVersionCodecResult result;
  result.status=error.status;result.diagnostic=MaterializeCatalogRecordDiagnostic(error);
  return result;
}
CatalogRecordDiagnosticView MetadataHashError(scratchbird::core::hash::Sha256PartsError error) {
  return {{StatusCode::platform_required_feature_missing,Severity::error,Subsystem::platform},
      "SB-CORE-HASH-SHA256-FAILED","core.hash.sha256_failed",
      scratchbird::core::hash::Sha256PartsErrorDetail(error),"core.hash.digest"};
}
template <typename Enum> bool MetadataEnum(Enum value, u16 maximum) {
  return static_cast<u16>(value) >= 1 && static_cast<u16>(value) <= maximum;
}
}

std::optional<CatalogRecordDiagnosticView> ValidateCatalogMetadataVersionView(const CatalogMetadataVersionView& value) {
  if (!MetadataEnum(value.authority_scope, 7) || !MetadataEnum(value.lifecycle, 10) ||
      !MetadataEnum(value.status, 9) || !MetadataEnum(value.visibility, 7))
    return MetadataViewError("enum_invalid");
  for (std::size_t i = 0; i < kMetadataCounters.size(); ++i)
    if (i != 3 && value.*kMetadataCounters[i] == 0) return MetadataViewError("counter_zero");
  const auto valid_key = [](std::string_view key, std::size_t maximum) {
    return !key.empty() && key.size() <= maximum && std::all_of(key.begin(), key.end(),
        [](unsigned char c) { return c >= 33 && c <= 126; });
  };
  if (!valid_key(value.trace_search_key, 4096) || !valid_key(value.object_subtype, 256) ||
      !valid_key(value.retention_class, 256))
    return MetadataViewError("trace_key_invalid");
  for (std::size_t i = 0; i < kMetadataReferences.size(); ++i) {
    const auto& id = value.*kMetadataReferences[i];
    if (!IsSuppliedIdentity(id)) {
      if (i == 0 || i == 1 || i == 9)
        return MetadataViewError("required_reference_missing");
    } else if (DurableIdentityError(id).has_value())
      return MetadataViewError("reference_invalid");
  }
  if (!IsTypedIdentity(value.creator_transaction_uuid, UuidKind::transaction) ||
      (IsSuppliedIdentity(value.retired_transaction_uuid) &&
       !IsTypedIdentity(value.retired_transaction_uuid, UuidKind::transaction)))
    return MetadataViewError("transaction_reference_kind");
  if (IsSuppliedIdentity(value.default_name_uuid) != IsSuppliedIdentity(value.name_vector_uuid))
    return MetadataViewError("name_vector_pair_invalid");
  if (IsSuppliedIdentity(value.owning_schema_uuid) && !IsTypedIdentity(value.owning_schema_uuid, UuidKind::schema))
    return MetadataViewError("owning_schema_kind_invalid");
  for (auto member : {&Metadata::default_name_uuid, &Metadata::name_vector_uuid,
                     &Metadata::security_policy_uuid, &Metadata::dependency_group_uuid,
                     &Metadata::storage_binding_uuid, &Metadata::donor_overlay_uuid})
    if (IsSuppliedIdentity(value.*member) && !IsTypedIdentity(value.*member, UuidKind::object))
      return MetadataViewError("object_reference_kind_invalid");
  if ((value.record.header.kind == CatalogRecordKind::metric_series ||
       value.object_subtype == "metric_series" || IsCatalogMetricSeriesPayload(value.record.payload)) &&
      !CatalogMetricSeriesMatchesMetadata(value))
    return MetadataViewError("metric_series_definition_binding_invalid");
  if ((value.record.header.kind == CatalogRecordKind::metric_label_schema ||
       value.object_subtype == "metric_label_schema" || IsCatalogMetricLabelSchemaPayload(value.record.payload)) &&
      !CatalogMetricLabelSchemaMatchesMetadata(value))
    return MetadataViewError("metric_label_schema_definition_binding_invalid");
  if ((value.record.header.kind == CatalogRecordKind::metric_descriptor ||
       value.object_subtype == "metric_descriptor" || IsCatalogMetricDescriptorPayload(value.record.payload)) &&
      !CatalogMetricDescriptorMatchesMetadata(value))
    return MetadataViewError("metric_descriptor_definition_binding_invalid");
  if ((value.object_subtype == "storage_action_attachment" ||
       IsCatalogStorageActionAttachmentPayload(value.record.payload)) &&
      !CatalogStorageActionAttachmentMatchesMetadata(value))
    return MetadataViewError("storage_action_attachment_binding_invalid");
  if ((value.object_subtype == "storage_action" ||
       IsCatalogStorageActionPolicyPayload(value.record.payload)) &&
      !CatalogStorageActionPolicyMatchesMetadata(value))
    return MetadataViewError("storage_action_definition_binding_invalid");
  if ((value.object_subtype == "agent_runtime_authority" ||
       IsCatalogRuntimeAuthorityBindingPayload(value.record.payload)) &&
      !CatalogRuntimeAuthorityBindingMatchesMetadata(value))
    return MetadataViewError("runtime_authority_definition_binding_invalid");
  if ((IsCatalogTableDefinitionPayload(value.record.payload) ||
       value.object_subtype == "ordinary_persistent_table") &&
      !CatalogTableDefinitionMatchesMetadata(value))
    return MetadataViewError("table_definition_binding_invalid");
  if ((IsCatalogColumnDefinitionPayload(value.record.payload) ||
       value.object_subtype == "persistent_table_column") &&
      !CatalogColumnDefinitionMatchesMetadata(value))
    return MetadataViewError("column_definition_binding_invalid");
  const bool retired = value.record.header.deleted;
  if ((value.object_subtype == "metric_visibility" ||
       IsCatalogMetricVisibilityPolicyPayload(value.record.payload)) &&
      !CatalogMetricVisibilityPolicyMatchesMetadata(value))
    return MetadataViewError("metric_visibility_definition_binding_invalid");
  if ((value.object_subtype == "metric_retention" ||
       IsCatalogMetricRetentionPolicyPayload(value.record.payload)) &&
      !CatalogMetricRetentionPolicyMatchesMetadata(value))
    return MetadataViewError("metric_retention_definition_binding_invalid");
  if (value.record.header.kind == CatalogRecordKind::schema &&
      !CatalogSchemaDefinitionMatchesMetadata(value))
    return MetadataViewError("schema_definition_binding_invalid");
  if ((value.object_subtype == "scheduler_runtime" ||
       IsCatalogSchedulerPolicyPayload(value.record.payload)) &&
      !CatalogSchedulerPolicyMatchesMetadata(value))
    return MetadataViewError("scheduler_runtime_definition_binding_invalid");
  if ((value.object_subtype == "scheduler_queues" ||
       IsCatalogSchedulerQueueProfilePayload(value.record.payload)) &&
      !CatalogSchedulerQueueProfileMatchesMetadata(value))
    return MetadataViewError("scheduler_queues_definition_binding_invalid");
  if ((value.object_subtype == "scheduler_fairness" ||
       IsCatalogSchedulerFairnessProfilePayload(value.record.payload)) &&
      !CatalogSchedulerFairnessProfileMatchesMetadata(value))
    return MetadataViewError("scheduler_fairness_definition_binding_invalid");
  if ((IsSuppliedIdentity(value.retired_transaction_uuid) &&
       ((value.status != CatalogObjectStatus::retired && value.status != CatalogObjectStatus::quarantined) ||
        value.retired_transaction_uuid.value != value.creator_transaction_uuid.value)) ||
      (retired && (!IsSuppliedIdentity(value.retired_transaction_uuid) ||
                   value.lifecycle != CatalogObjectLifecycle::dropped ||
                   value.status != CatalogObjectStatus::retired ||
                   value.retired_transaction_uuid.value != value.creator_transaction_uuid.value)))
    return MetadataViewError("retirement_binding_invalid");
  const auto inner=ValidateCatalogTypedRecordView(value.record);
  if(!inner.ok())return inner.diagnostic;
  const auto inner_size=kBinaryHeaderBytes+value.record.payload.size();
  const auto text_size=value.trace_search_key.size()+value.object_subtype.size()+value.retention_class.size();
  if(inner_size>kMetadataMaxBytes-kMetadataHeaderBytes-text_size)return MetadataViewError("size_limit");
  return std::nullopt;
}

std::optional<std::size_t> CatalogMetadataVersionEncodedBytes(const CatalogMetadataVersionView& v) {
  if(ValidateCatalogMetadataVersionView(v))return {};
  return kMetadataHeaderBytes+kBinaryHeaderBytes+v.record.payload.size()+v.trace_search_key.size()+
      v.object_subtype.size()+v.retention_class.size();
}
CatalogMetadataVersionCodecResult EncodeCatalogMetadataVersion(const CatalogMetadataVersion& value) {
  const auto view=BorrowCatalogMetadataVersion(value);
  if(const auto error=ValidateCatalogMetadataVersionView(view))return MetadataFailure(*error);
  const auto inner_size=kBinaryHeaderBytes+value.record.payload.size();
  const auto text_size=value.trace_search_key.size()+value.object_subtype.size()+value.retention_class.size();
  CatalogMetadataVersionCodecResult result;
  result.bytes.assign(kMetadataHeaderBytes + text_size + inner_size, 0);
  auto* out = result.bytes.data();
  std::memcpy(out, "SBCMV001", 8);
  StoreLittle16(out + 8, 1); StoreLittle16(out + 10, kMetadataHeaderBytes);
  StoreLittle32(out + 12, static_cast<u32>(result.bytes.size()));
  StoreLittle16(out + 16, static_cast<u16>(value.authority_scope));
  StoreLittle16(out + 18, static_cast<u16>(value.lifecycle));
  StoreLittle16(out + 20, static_cast<u16>(value.status));
  StoreLittle16(out + 22, static_cast<u16>(value.visibility));
  for (std::size_t i = 0; i < kMetadataCounters.size(); ++i)
    StoreLittle64(out + 32 + 8 * i, view.*kMetadataCounters[i]);
  for (std::size_t i = 0; i < kMetadataReferences.size(); ++i) {
    const auto& id = view.*kMetadataReferences[i];
    std::copy(id.value.bytes.begin(), id.value.bytes.end(), out + 96 + 16 * i);
    out[272 + i] = static_cast<byte>(id.kind);
  }
  auto* nested=out+kMetadataHeaderBytes+text_size;
  WriteTypedRecord(view.record,nested);
  const auto definition=scratchbird::core::hash::ComputeSha256Digest(nested,inner_size);
  if(!definition.ok()) {
    CatalogMetadataVersionCodecResult failure;
    failure.status=definition.status;failure.diagnostic=definition.diagnostic;return failure;
  }
  std::copy(definition.digest.begin(), definition.digest.end(), out + 288);
  StoreLittle32(out + 352, static_cast<u32>(value.trace_search_key.size()));
  StoreLittle32(out + 356, static_cast<u32>(inner_size));
  StoreLittle32(out + 360, static_cast<u32>(value.object_subtype.size()));
  StoreLittle32(out + 364, static_cast<u32>(value.retention_class.size()));
  std::copy(value.trace_search_key.begin(), value.trace_search_key.end(), out + kMetadataHeaderBytes);
  std::copy(value.object_subtype.begin(), value.object_subtype.end(), out + kMetadataHeaderBytes + value.trace_search_key.size());
  std::copy(value.retention_class.begin(), value.retention_class.end(), out + kMetadataHeaderBytes + value.trace_search_key.size() + value.object_subtype.size());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(result.bytes);
  if (!digest.ok()) {
    CatalogMetadataVersionCodecResult failure;
    failure.status = digest.status; failure.diagnostic = digest.diagnostic; return failure;
  }
  std::copy(digest.digest.begin(), digest.digest.end(), out + 320);
  result.status = CodecOkStatus(); result.record = value; result.definition_sha256 = definition.digest;
  return result;
}

CatalogMetadataVersionViewResult DecodeCatalogMetadataVersionView(std::span<const byte> bytes) {
  const auto failure=[](CatalogRecordDiagnosticView error) {
    return CatalogMetadataVersionViewResult{{},{},error};
  };
  const auto invalid=[&](const char* detail) { return failure(MetadataViewError(detail)); };
  if(bytes.size()<kMetadataHeaderBytes || bytes.size()>kMetadataMaxBytes)return invalid("size_invalid");
  const auto* in=bytes.data();
  if(std::memcmp(in,"SBCMV001",8) || LoadLittle16(in+8)!=1 ||
      LoadLittle16(in+10)!=kMetadataHeaderBytes || LoadLittle32(in+12)!=bytes.size())
    return invalid("header_invalid");
  const auto trace_size=LoadLittle32(in+352),inner_size=LoadLittle32(in+356);
  const auto subtype_size=LoadLittle32(in+360),retention_size=LoadLittle32(in+364);
  const auto text_size=static_cast<u64>(trace_size)+subtype_size+retention_size;
  if(trace_size>4096 || subtype_size>256 || retention_size>256 ||
      static_cast<u64>(kMetadataHeaderBytes)+text_size+inner_size!=bytes.size())
    return invalid("length_invalid");
  constexpr std::array<byte,32> zero{};
  const scratchbird::core::hash::HashDigestSegment parts[]{
      {in,320},{zero.data(),zero.size()},{in+352,bytes.size()-352}};
  const auto digest=scratchbird::core::hash::ComputeSha256DigestPartsNative(parts,3);
  if(!digest.ok())return failure(MetadataHashError(digest.error));
  if(!std::equal(digest.digest.begin(),digest.digest.end(),in+320))
    return invalid("version_digest_mismatch");
  CatalogMetadataVersionView value;
  value.authority_scope=static_cast<CatalogAuthorityScope>(LoadLittle16(in+16));
  value.lifecycle=static_cast<CatalogObjectLifecycle>(LoadLittle16(in+18));
  value.status=static_cast<CatalogObjectStatus>(LoadLittle16(in+20));
  value.visibility=static_cast<CatalogVisibilityClass>(LoadLittle16(in+22));
  for(std::size_t i=0;i<kMetadataCounters.size();++i)value.*kMetadataCounters[i]=LoadLittle64(in+32+8*i);
  for(std::size_t i=0;i<kMetadataReferences.size();++i) {
    auto& id=value.*kMetadataReferences[i];id.kind=static_cast<UuidKind>(in[272+i]);
    std::copy(in+96+16*i,in+112+16*i,id.value.bytes.begin());
  }
  value.trace_search_key={reinterpret_cast<const char*>(in+kMetadataHeaderBytes),trace_size};
  value.object_subtype={reinterpret_cast<const char*>(in+kMetadataHeaderBytes+trace_size),subtype_size};
  value.retention_class={reinterpret_cast<const char*>(in+kMetadataHeaderBytes+trace_size+subtype_size),retention_size};
  const auto* nested=in+kMetadataHeaderBytes+text_size;
  const auto inner=DecodeCatalogTypedRecordView(CatalogPageRowKind::typed_catalog_record,
      {reinterpret_cast<const char*>(nested),inner_size});
  if(!inner.ok())return failure(inner.diagnostic);
  value.record=*inner.record;
  if(const auto error=ValidateCatalogMetadataVersionView(value))return failure(*error);
  const scratchbird::core::hash::HashDigestSegment definition_part{nested,inner_size};
  const auto definition=scratchbird::core::hash::ComputeSha256DigestPartsNative(&definition_part,1);
  if(!definition.ok())return failure(MetadataHashError(definition.error));
  const auto zeros=[](const byte* first,const byte* last) {
    return std::all_of(first,last,[](byte b){return b==0;});
  };
  // Common/nested fields have exact canonical encodings. These are the only
  // remaining bytes the old re-encode comparison could reject.
  if(!std::equal(definition.digest.begin(),definition.digest.end(),in+288) ||
      !zeros(in+24,in+32) || !zeros(in+283,in+288) || !zeros(in+368,in+384))
    return invalid("digest_reserved_or_canonical_mismatch");
  return {value,definition.digest,{}};
}

CatalogMetadataVersion MaterializeCatalogMetadataVersion(const CatalogMetadataVersionView& v) {
  return {{v.record.header,std::string(v.record.payload)},
      v.authority_scope,
      v.lifecycle,
      v.status,
      v.visibility,
      v.definition_version,
      v.schema_epoch,
      v.security_epoch,
      v.resource_epoch,
      v.catalog_generation,
      v.dependency_generation,
      v.invalidation_generation,
      v.creator_local_transaction_id,
      v.owner_uuid,
      v.creator_transaction_uuid,
      v.retired_transaction_uuid,
      v.default_name_uuid,
      v.name_vector_uuid,
      v.security_policy_uuid,
      v.dependency_group_uuid,
      v.storage_binding_uuid,
      v.donor_overlay_uuid,
      v.audit_uuid,
      v.owning_schema_uuid,
      std::string(v.trace_search_key),
      std::string(v.object_subtype),
      std::string(v.retention_class)};
}

CatalogMetadataVersionCodecResult DecodeCatalogMetadataVersion(const std::vector<byte>& bytes) {
  const auto decoded=DecodeCatalogMetadataVersionView(bytes);
  if(!decoded.ok())return MetadataFailure(decoded.diagnostic);
  CatalogMetadataVersionCodecResult result;
  result.record=MaterializeCatalogMetadataVersion(*decoded.record);
  result.bytes=bytes;
  result.definition_sha256=decoded.definition_sha256;
  result.status=CodecOkStatus();
  return result;
}

DiagnosticRecord MakeCatalogRecordCodecDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.catalog.record_codec");
}

bool CatalogMetadataPreservesFamilyOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor) {
  return CatalogRuntimeAuthorityBindingPreservesOrigin(previous,successor) &&
      CatalogSchedulerPolicyPreservesOrigin(previous,successor) &&
      CatalogSchedulerQueueProfilePreservesOrigin(previous,successor) &&
      CatalogSchedulerFairnessProfilePreservesOrigin(previous,successor) &&
      CatalogSchemaDefinitionPreservesOrigin(previous,successor) &&
      CatalogTableDefinitionPreservesOrigin(previous,successor) &&
      CatalogColumnDefinitionPreservesOrigin(previous,successor) &&
      CatalogStorageActionPolicyPreservesOrigin(previous,successor) &&
      CatalogStorageActionAttachmentPreservesOrigin(previous,successor) &&
      CatalogMetricRetentionPolicyPreservesOrigin(previous,successor) &&
      CatalogMetricVisibilityPolicyPreservesOrigin(previous,successor) &&
      CatalogMetricDescriptorPreservesOrigin(previous,successor) &&
      CatalogMetricLabelSchemaPreservesOrigin(previous,successor) &&
      CatalogMetricSeriesPreservesOrigin(previous,successor);
}

bool CatalogMetadataPreservesFamilyOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  return CatalogMetadataPreservesFamilyOrigin(
      BorrowCatalogMetadataVersion(previous),BorrowCatalogMetadataVersion(successor));
}

}  // namespace scratchbird::core::catalog
