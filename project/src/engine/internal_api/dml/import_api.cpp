// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB_ENGINE_INTERNAL_API_DML_IMPORT_API_BEHAVIOR
// SB_ENGINE_BOUND_IMPORT_DESCRIPTOR_REGISTRY_V1

#include "dml/import_api.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "api_diagnostics.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "security/security_model.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "storage/database/local_transaction_store.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
namespace {

namespace codec = scratchbird::engine::sblr;

constexpr std::string_view kOperationId = "dml.plan_import_rows";
constexpr std::string_view kBinderTag =
    "private_dml_plan_import_rows_binder";
constexpr std::string_view kConsumerTag =
    "private_dml_plan_import_rows_consumer";
constexpr std::string_view kClusterGatewayTag =
    "private_dml_plan_import_rows_cluster_gateway";

constexpr const char* kOpcodeInvalid = "SBLR.OPCODE_INVALID";
constexpr const char* kOperandInvalid = "SBLR.OPERAND_INVALID";
constexpr const char* kAccessDenied = "SECURITY.ACCESS_DENIED";
constexpr const char* kTransactionInvalid = "MGA.TRANSACTION_INVALID";
constexpr const char* kAuthorityMismatch = "MGA.AUTHORITY_MISMATCH";
constexpr const char* kClusterFallthrough =
    "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN";
constexpr const char* kCancelled = "PROCESS.CANCELLED";
constexpr const char* kExecutorEvidenceMissing =
    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
constexpr const char* kUnsupported = "SBLR.OPERATION_UNSUPPORTED";

using RegistryKey =
    std::tuple<codec::PlanImportRowsUuidV1, std::uint64_t,
               codec::PlanImportRowsUuidV1, std::uint64_t>;
using RegistryHeadKey =
    std::tuple<codec::PlanImportRowsUuidV1, std::uint64_t,
               codec::PlanImportRowsUuidV1>;
using RegistryPrimaryKey =
    std::pair<codec::PlanImportRowsUuidV1, std::uint64_t>;

struct BoundImportDescriptorRegistryV1 {
  std::shared_mutex mutex;
  std::map<RegistryKey, EngineBoundImportRowsPlanDescriptorV1> rows;
  std::map<RegistryHeadKey, std::uint64_t> current_heads;
  std::map<RegistryPrimaryKey, RegistryKey> primary_rows;
};

BoundImportDescriptorRegistryV1& BoundRegistry() {
  static BoundImportDescriptorRegistryV1 registry;
  return registry;
}

EngineApiDiagnostic Diagnostic(std::string code,
                               std::string message_key,
                               std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(message_key),
                                 std::move(detail), true);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

bool HasTraceTag(const EngineRequestContext& context, std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

template <std::size_t N>
bool NonzeroBytes(const std::array<std::uint8_t, N>& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool CanonicalUuidBytes(std::string_view text,
                        codec::PlanImportRowsUuidV1* out) {
  if (out == nullptr || text.empty()) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) {
    return false;
  }
  *out = parsed.value.bytes;
  return true;
}

bool UuidTextEquals(std::string_view text,
                    const codec::PlanImportRowsUuidV1& expected) {
  codec::PlanImportRowsUuidV1 actual{};
  return CanonicalUuidBytes(text, &actual) && actual == expected;
}

std::string UuidText(const codec::PlanImportRowsUuidV1& value) {
  scratchbird::core::platform::Uuid uuid;
  uuid.bytes = value;
  return scratchbird::core::uuid::UuidToString(uuid);
}

std::string Hex(const codec::PlanImportRowsSha256V1& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (const std::uint8_t byte : value) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

std::string CanonicalDiagnosticCode(std::string code,
                                    std::string fallback) {
  if (code == kOpcodeInvalid || code == kOperandInvalid ||
      code == kAccessDenied || code == kTransactionInvalid ||
      code == kAuthorityMismatch ||
      code == kClusterFallthrough || code == kCancelled ||
      code == kExecutorEvidenceMissing || code == kUnsupported) {
    return code;
  }
  return fallback;
}

EngineApiDiagnostic CodecDiagnostic(
    const codec::PlanImportRowsCodecDiagnosticV1& source,
    std::string_view phase,
    std::string fallback = kOperandInvalid) {
  return Diagnostic(CanonicalDiagnosticCode(source.code, std::move(fallback)),
                    "sblr.dml.plan_import_rows." + std::string(phase),
                    "canonical_" + std::string(phase) + "_refused");
}

EnginePlanImportRowsResult ImportFailure(EngineApiDiagnostic diagnostic) {
  EnginePlanImportRowsResult result;
  result.ok = false;
  result.operation_id = std::string(kOperationId);
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

EnginePlanImportRowsResult ImportFailure(std::string code,
                                         std::string message_key,
                                         std::string detail) {
  return ImportFailure(Diagnostic(std::move(code), std::move(message_key),
                                  std::move(detail)));
}

bool CancellationObserved(const EngineRequestContext& context) {
  return context.query_cancellation_requested &&
         context.query_cancellation_requested();
}

bool BinderContextAdmitted(const EngineRequestContext& context) {
  return HasTraceTag(context, kBinderTag) &&
         !HasTraceTag(context, kConsumerTag) &&
         context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         context.authorization_context.present &&
         !context.authorization_context.authority_uuid.canonical.empty() &&
         context.authorization_context.security_context_generation != 0;
}

bool ConsumerContextAdmitted(const EngineRequestContext& context) {
  return HasTraceTag(context, kConsumerTag) &&
         !HasTraceTag(context, kBinderTag) &&
         context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         context.authorization_context.present &&
         !context.authorization_context.authority_uuid.canonical.empty() &&
         context.authorization_context.security_context_generation != 0;
}

bool LiveAuthorityEqual(const codec::PlanImportRowsLiveAuthorityV1& left,
                        const codec::PlanImportRowsLiveAuthorityV1& right) {
  return left.authenticated_statement_receipt_uuid ==
             right.authenticated_statement_receipt_uuid &&
         left.structural_occurrence_id == right.structural_occurrence_id &&
         left.local_transaction_id == right.local_transaction_id &&
         left.transaction_uuid == right.transaction_uuid &&
         left.mga_snapshot_uuid == right.mga_snapshot_uuid &&
         left.mga_snapshot_generation == right.mga_snapshot_generation &&
         left.security_snapshot_uuid == right.security_snapshot_uuid &&
         left.security_snapshot_generation ==
             right.security_snapshot_generation &&
         left.policy_snapshot_uuid == right.policy_snapshot_uuid &&
         left.policy_snapshot_generation == right.policy_snapshot_generation &&
         left.resource_admission_uuid == right.resource_admission_uuid &&
         left.resource_admission_generation ==
             right.resource_admission_generation &&
         left.catalog_generation == right.catalog_generation &&
         left.target_table_uuid == right.target_table_uuid &&
         left.target_relation_descriptor_uuid ==
             right.target_relation_descriptor_uuid &&
         left.target_relation_descriptor_generation ==
             right.target_relation_descriptor_generation &&
         left.executor_availability_generation ==
             right.executor_availability_generation;
}

bool HasInsertAuthorization(
    const EngineRequestContext& context,
    const codec::PlanImportRowsUuidV1& target_table_uuid) {
  if (!context.security_context_present ||
      !context.authorization_context.present) {
    return false;
  }
  const auto decision = EvaluateMaterializedAuthorization(
      context, context.authorization_context, "INSERT",
      UuidText(target_table_uuid));
  return decision.authorized;
}

bool ActiveTransactionIdentity(const EngineRequestContext& context) {
  using scratchbird::core::platform::UuidKind;
  using scratchbird::transaction::mga::LookupLocalTransaction;
  using scratchbird::transaction::mga::MakeLocalTransactionId;
  using scratchbird::transaction::mga::TransactionState;
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return false;
  }
  const auto transaction_uuid = scratchbird::core::uuid::ParseTypedUuid(
      UuidKind::transaction, context.transaction_uuid.canonical);
  if (!transaction_uuid.ok()) return false;
  const auto loaded = scratchbird::storage::database::
      LoadLocalTransactionInventoryFromDatabase(context.database_path);
  if (!loaded.ok()) return false;
  const auto found = LookupLocalTransaction(
      loaded.inventory, MakeLocalTransactionId(context.local_transaction_id));
  return found.ok() &&
         found.entry.identity.transaction_uuid.value ==
             transaction_uuid.value.value &&
         (found.entry.state == TransactionState::active ||
          found.entry.state == TransactionState::read_only_active);
}

SblrExecutorAvailabilityRowIdentity PlanImportRowsAvailabilityIdentity() {
  SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = kSblrDmlPlanImportRowsExecutorId;
  identity.opcode_code = kSblrDmlPlanImportRowsOpcodeCode;
  identity.opcode_version = kSblrDmlPlanImportRowsOpcodeVersion;
  identity.operand_descriptor_id = kSblrDmlPlanImportRowsOperandDescriptorId;
  identity.result_descriptor_id = kSblrDmlPlanImportRowsResultDescriptorId;
  identity.result_descriptor_version =
      kSblrDmlPlanImportRowsResultDescriptorVersion;
  return identity;
}

bool LoadCurrentPlanImportRowsAvailability(
    const EngineRequestContext& context,
    std::uint64_t* generation) {
  if (generation == nullptr) return false;
  const auto loaded = LoadCurrentSblrExecutorAvailabilitySnapshot(
      context, PlanImportRowsAvailabilityIdentity());
  if (!loaded.ok || !loaded.snapshot.installed ||
      loaded.snapshot.availability_state !=
          SblrExecutorAvailabilityState::installed ||
      loaded.snapshot.generation == 0) {
    return false;
  }
  *generation = loaded.snapshot.generation;
  return true;
}

struct LiveAuthorityResolutionV1 {
  bool ok = false;
  codec::PlanImportRowsLiveAuthorityV1 live;
  std::string detail;
};

// Derives every v1 live field from engine-owned context and current loaders.
// The expected executor generation is supplied only to keep the executor gate
// at its later canonical precedence position; callers must independently load
// and compare the current 793 availability row at that position.
LiveAuthorityResolutionV1 ResolveLiveImportRowsAuthority(
    const EngineRequestContext& context,
    std::uint64_t structural_occurrence_id,
    std::string_view target_table_uuid,
    std::uint64_t expected_executor_availability_generation) {
  LiveAuthorityResolutionV1 result;
  auto& live = result.live;
  const auto& authorization = context.authorization_context;
  if (structural_occurrence_id == 0 || context.local_transaction_id == 0 ||
      context.statement_snapshot_generation == 0 ||
      context.transaction_policy_snapshot_generation == 0 ||
      authorization.security_context_generation == 0 ||
      context.resource_epoch == 0 || context.catalog_generation_id == 0 ||
      expected_executor_availability_generation == 0 ||
      authorization.security_epoch == 0 ||
      authorization.security_epoch != context.security_epoch ||
      authorization.policy_epoch == 0 ||
      authorization.catalog_generation_id == 0 ||
      authorization.catalog_generation_id != context.catalog_generation_id ||
      !CanonicalUuidBytes(context.statement_receipt_uuid.canonical,
                          &live.authenticated_statement_receipt_uuid) ||
      !CanonicalUuidBytes(context.transaction_uuid.canonical,
                          &live.transaction_uuid) ||
      !CanonicalUuidBytes(context.statement_snapshot_uuid.canonical,
                          &live.mga_snapshot_uuid) ||
      !CanonicalUuidBytes(authorization.authority_uuid.canonical,
                          &live.security_snapshot_uuid) ||
      !CanonicalUuidBytes(context.transaction_policy_snapshot_uuid.canonical,
                          &live.policy_snapshot_uuid) ||
      !CanonicalUuidBytes(context.resource_admission_uuid.canonical,
                          &live.resource_admission_uuid) ||
      !CanonicalUuidBytes(target_table_uuid, &live.target_table_uuid)) {
    result.detail = "complete_engine_live_authority_required";
    return result;
  }

  EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = context;
  const auto snapshot = EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok ||
      snapshot.statement_snapshot_uuid.canonical !=
          context.statement_snapshot_uuid.canonical ||
      !snapshot.snapshot_vector.inventory_authoritative ||
      !snapshot.snapshot_vector.complete ||
      snapshot.snapshot_vector
              .publication_inventory_next_local_transaction_id !=
          context.statement_snapshot_generation) {
    result.detail = "current_statement_snapshot_required";
    return result;
  }

  const auto relation = LoadMgaRelationStorageDescriptor(
      context, std::string(target_table_uuid));
  if (!relation.ok ||
      ValidateMgaRelationStorageDescriptor(relation.descriptor).error ||
      !UuidTextEquals(relation.descriptor.relation_uuid.canonical,
                      live.target_table_uuid) ||
      !CanonicalUuidBytes(relation.descriptor.descriptor_uuid.canonical,
                          &live.target_relation_descriptor_uuid) ||
      relation.descriptor.descriptor_generation == 0) {
    result.detail = "current_target_relation_descriptor_required";
    return result;
  }

  live.structural_occurrence_id = structural_occurrence_id;
  live.local_transaction_id = context.local_transaction_id;
  live.mga_snapshot_generation = context.statement_snapshot_generation;
  live.security_snapshot_generation =
      authorization.security_context_generation;
  live.policy_snapshot_generation =
      context.transaction_policy_snapshot_generation;
  live.resource_admission_generation = context.resource_epoch;
  live.catalog_generation = context.catalog_generation_id;
  live.target_relation_descriptor_generation =
      relation.descriptor.descriptor_generation;
  live.executor_availability_generation =
      expected_executor_availability_generation;
  result.ok = true;
  return result;
}

bool MappingDemandShapeValid(
    const std::vector<EngineImportRowsColumnMappingDemandV1>& demands) {
  if (demands.size() > codec::kPlanImportRowsMaximumMappingsV1) return false;
  std::set<codec::PlanImportRowsUuidV1> target_columns;
  std::uint32_t previous_source_ordinal = 0;
  for (std::size_t index = 0; index < demands.size(); ++index) {
    codec::PlanImportRowsUuidV1 target_column{};
    if ((index == 0 && demands[index].source_field_ordinal != 0) ||
        (index != 0 &&
         demands[index].source_field_ordinal <= previous_source_ordinal) ||
        !CanonicalUuidBytes(demands[index].target_column_uuid.canonical,
                            &target_column) ||
        !target_columns.insert(target_column).second) {
      return false;
    }
    previous_source_ordinal = demands[index].source_field_ordinal;
  }
  return true;
}

bool LegacyPlanningAuthorityPresent(const EnginePlanImportRowsRequest& request) {
  const auto& source = request.source;
  const auto& format = request.format;
  const auto& policy = request.import_policy;
  const auto& sql_name = request.sql_object_reference;
  return !request.localized_names.empty() ||
         !request.option_envelopes.empty() ||
         !request.diagnostic_options.empty() ||
         !request.target_table.uuid.canonical.empty() ||
         !request.target_table.object_kind.empty() ||
         !request.target_object.uuid.canonical.empty() ||
         !request.target_object.object_kind.empty() ||
         !request.related_objects.empty() || !request.descriptors.empty() ||
         !request.rows.empty() || !request.assignments.empty() ||
         !request.column_mappings.empty() || !source.source_kind.empty() ||
         !source.source_uuid.canonical.empty() ||
         !source.source_fingerprint.empty() || !source.source_position.empty() ||
         !source.redacted_source_handle.empty() ||
         !format.format_family.empty() || !format.encoding.empty() ||
         !format.line_ending.empty() || !format.delimiter.empty() ||
         !format.quote.empty() || !format.escape.empty() ||
         !format.header_policy.empty() || !format.null_markers.empty() ||
         !format.date_time_profile.empty() || !format.timezone_profile.empty() ||
         !format.format_options.empty() ||
         policy.reject_mode != "fail_fast" || policy.reject_limit_rows != 0 ||
         policy.reject_limit_percent != 0.0 ||
         !policy.reject_target.uuid.canonical.empty() ||
         policy.reject_payload_policy != "diagnostic_only" ||
         policy.resume_policy != "fail_closed" ||
         policy.strict_bulk_load_requested ||
         policy.reference_relaxed_semantics_requested ||
         !sql_name.path_components.empty() ||
         !sql_name.object_name.raw_text.empty() ||
         !sql_name.object_name.normalized_lookup_key.empty();
}

RegistryKey MakeRegistryKey(
    const codec::PlanImportRowsUuidV1& receipt_uuid,
    std::uint64_t structural_occurrence_id,
    const codec::PlanImportRowsDescriptorRefV1& descriptor_ref) {
  return {receipt_uuid, structural_occurrence_id, descriptor_ref.descriptor_uuid,
          descriptor_ref.descriptor_generation};
}

RegistryHeadKey MakeRegistryHeadKey(
    const codec::PlanImportRowsUuidV1& receipt_uuid,
    std::uint64_t structural_occurrence_id,
    const codec::PlanImportRowsUuidV1& descriptor_uuid) {
  return {receipt_uuid, structural_occurrence_id, descriptor_uuid};
}

const char* SourceKindText(codec::PlanImportRowsSourceKindV1 value) {
  switch (value) {
    case codec::PlanImportRowsSourceKindV1::native_sbsql_import:
      return "native_sbsql_import";
    case codec::PlanImportRowsSourceKindV1::csv_stream:
      return "csv_stream";
    case codec::PlanImportRowsSourceKindV1::delimited_text:
      return "delimited_text";
    case codec::PlanImportRowsSourceKindV1::fixed_width_text:
      return "fixed_width_text";
    case codec::PlanImportRowsSourceKindV1::jsonl_stream:
      return "jsonl_stream";
    case codec::PlanImportRowsSourceKindV1::document_stream:
      return "document_stream";
    case codec::PlanImportRowsSourceKindV1::binary_typed_rows:
      return "binary_typed_rows";
    case codec::PlanImportRowsSourceKindV1::reference_dump_replay:
      return "reference_dump_replay";
    case codec::PlanImportRowsSourceKindV1::reference_bulk_api:
      return "reference_bulk_api";
    case codec::PlanImportRowsSourceKindV1::live_ingest_stream:
      return "live_ingest_stream";
    case codec::PlanImportRowsSourceKindV1::bulk_import_job:
      return "bulk_import_job";
    case codec::PlanImportRowsSourceKindV1::xml_stream:
      return "xml_stream";
    case codec::PlanImportRowsSourceKindV1::line_protocol_stream:
      return "line_protocol_stream";
  }
  return "";
}

const char* FormatFamilyText(codec::PlanImportRowsFormatFamilyV1 value) {
  switch (value) {
    case codec::PlanImportRowsFormatFamilyV1::csv:
      return "csv";
    case codec::PlanImportRowsFormatFamilyV1::delimited_text:
      return "delimited_text";
    case codec::PlanImportRowsFormatFamilyV1::fixed_width:
      return "fixed_width";
    case codec::PlanImportRowsFormatFamilyV1::jsonl:
      return "jsonl";
    case codec::PlanImportRowsFormatFamilyV1::document:
      return "document";
    case codec::PlanImportRowsFormatFamilyV1::binary_typed_rows:
      return "binary_typed_rows";
    case codec::PlanImportRowsFormatFamilyV1::reference_dump:
      return "reference_dump";
    case codec::PlanImportRowsFormatFamilyV1::reference_bulk:
      return "reference_bulk";
    case codec::PlanImportRowsFormatFamilyV1::live_ingest:
      return "live_ingest";
    case codec::PlanImportRowsFormatFamilyV1::xml:
      return "xml";
    case codec::PlanImportRowsFormatFamilyV1::line_protocol:
      return "line_protocol";
    case codec::PlanImportRowsFormatFamilyV1::bulk_job:
      return "bulk_job";
  }
  return "";
}

const char* InsertModeText(codec::PlanImportRowsInsertModeV1 value) {
  switch (value) {
    case codec::PlanImportRowsInsertModeV1::copy_import:
      return "copy_import";
    case codec::PlanImportRowsInsertModeV1::native_bulk:
      return "native_bulk";
    case codec::PlanImportRowsInsertModeV1::reference_bulk:
      return "reference_bulk";
  }
  return "";
}

bool ClusterPredicate(const EngineRequestContext& context) {
  return !context.cluster_uuid.canonical.empty() ||
         context.cluster_transaction_active || context.route_fence_present;
}

bool ClusterRouteAdmitted(const EnginePlanImportRowsRequest& request) {
  if (!ClusterPredicate(request.context)) return true;
  return request.context.cluster_authority_available &&
         HasTraceTag(request.context, kClusterGatewayTag) &&
         request.cluster_context_execution_validated &&
         request.cluster_read_route_security_validated &&
         request.cluster_read_only_evidence_validated;
}

bool ExactFrozenCarrierSet(
    const codec::PlanImportRowsCarrierSetV1& carriers,
    codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> descriptor;
  std::vector<std::uint8_t> source;
  std::vector<std::uint8_t> format;
  std::vector<std::uint8_t> mapping;
  std::vector<std::uint8_t> policy;
  return codec::EncodePlanImportRowsSourceEnvelopeV1(carriers.source, &source,
                                                      diagnostic) &&
         source == carriers.source.exact_bytes &&
         codec::EncodePlanImportRowsFormatEnvelopeV1(carriers.format, &format,
                                                      diagnostic) &&
         format == carriers.format.exact_bytes &&
         codec::EncodePlanImportRowsMappingVectorV1(carriers.mapping, &mapping,
                                                     diagnostic) &&
         mapping == carriers.mapping.exact_bytes &&
         codec::EncodePlanImportRowsPolicyV1(carriers.policy, &policy,
                                             diagnostic) &&
         policy == carriers.policy.exact_bytes &&
         codec::EncodePlanImportRowsPlanDescriptorV1(carriers.descriptor,
                                                      &descriptor,
                                                      diagnostic) &&
         descriptor == carriers.descriptor.exact_bytes;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool IssueBinderUuid(codec::PlanImportRowsUuidV1* out) {
  if (out == nullptr) return false;
  const auto issued = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, CurrentUnixMillis());
  if (!issued.ok()) return false;
  *out = issued.value.value.bytes;
  return NonzeroBytes(*out);
}

codec::PlanImportRowsChildRefV1 ChildReference(
    const codec::PlanImportRowsNestedHeaderV1& header) {
  codec::PlanImportRowsChildRefV1 reference;
  reference.row_uuid = header.row_uuid;
  reference.row_generation = header.row_generation;
  reference.canonical_row_sha256 = header.row_sha256;
  return reference;
}

bool FreezeSource(codec::PlanImportRowsSourceEnvelopeV1* value,
                  codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> bytes;
  if (value == nullptr ||
      !codec::EncodePlanImportRowsSourceEnvelopeV1(*value, &bytes,
                                                   diagnostic)) {
    return false;
  }
  return codec::DecodePlanImportRowsSourceEnvelopeV1(
      bytes.data(), bytes.size(), value, diagnostic);
}

bool FreezeFormat(codec::PlanImportRowsFormatEnvelopeV1* value,
                  codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> bytes;
  if (value == nullptr ||
      !codec::EncodePlanImportRowsFormatEnvelopeV1(*value, &bytes,
                                                   diagnostic)) {
    return false;
  }
  return codec::DecodePlanImportRowsFormatEnvelopeV1(
      bytes.data(), bytes.size(), value, diagnostic);
}

bool FreezeMapping(codec::PlanImportRowsMappingVectorV1* value,
                   codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> bytes;
  if (value == nullptr ||
      !codec::EncodePlanImportRowsMappingVectorV1(*value, &bytes,
                                                  diagnostic)) {
    return false;
  }
  return codec::DecodePlanImportRowsMappingVectorV1(
      bytes.data(), bytes.size(), value, diagnostic);
}

bool FreezePolicy(codec::PlanImportRowsPolicyV1* value,
                  codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> bytes;
  if (value == nullptr ||
      !codec::EncodePlanImportRowsPolicyV1(*value, &bytes, diagnostic)) {
    return false;
  }
  return codec::DecodePlanImportRowsPolicyV1(bytes.data(), bytes.size(), value,
                                              diagnostic);
}

bool FreezeDescriptor(codec::PlanImportRowsPlanDescriptorV1* value,
                      codec::PlanImportRowsCodecDiagnosticV1* diagnostic) {
  std::vector<std::uint8_t> bytes;
  if (value == nullptr ||
      !codec::EncodePlanImportRowsPlanDescriptorV1(*value, &bytes,
                                                   diagnostic)) {
    return false;
  }
  return codec::DecodePlanImportRowsPlanDescriptorV1(
      bytes.data(), bytes.size(), value, diagnostic);
}

}  // namespace

EngineBindImportRowsPlanDescriptorResultV1
PublishEngineBoundImportRowsPlanDescriptorV1(
    const EngineBindImportRowsPlanDescriptorRequestV1& request) {
  EngineBindImportRowsPlanDescriptorResultV1 result;
  if (!BinderContextAdmitted(request.context)) {
    result.diagnostic = Diagnostic(
        kAccessDenied, "sblr.dml.plan_import_rows.binder_access_denied",
        "engine_import_binder_authority_required");
    return result;
  }

  codec::PlanImportRowsCodecDiagnosticV1 codec_diagnostic;
  if (!ExactFrozenCarrierSet(request.row.carriers, &codec_diagnostic)) {
    result.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_carrier_not_frozen",
        "byte_identical_frozen_carrier_set_required");
    return result;
  }
  codec_diagnostic = {};
  const bool carrier_admitted = codec::ValidatePlanImportRowsCarrierSetV1(
      request.row.carriers, &codec_diagnostic);
  if (!carrier_admitted && codec_diagnostic.code != kUnsupported) {
    result.diagnostic =
        CodecDiagnostic(codec_diagnostic, "binder_carrier_validation");
    return result;
  }
  codec_diagnostic = {};
  const bool policy_admitted = codec::ValidatePlanImportRowsPolicyAdmissionV1(
      request.row.carriers.policy,
      request.row.reference_relaxed_semantics_authorized, &codec_diagnostic);
  if (!policy_admitted && codec_diagnostic.code != kUnsupported) {
    result.diagnostic =
        CodecDiagnostic(codec_diagnostic, "binder_policy_validation");
    return result;
  }
  if (request.structural_occurrence_id == 0 ||
      request.structural_occurrence_id !=
          request.live_authority.structural_occurrence_id ||
      request.structural_occurrence_id !=
          request.row.carriers.descriptor.structural_occurrence_id ||
      !NonzeroBytes(request.row.executor_evidence_uuid) ||
      request.row.executor_evidence_generation == 0) {
    result.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_row_invalid",
        "complete_immutable_binder_row_required");
    return result;
  }
  if (!HasInsertAuthorization(request.context,
                              request.row.carriers.descriptor.target_table_uuid)) {
    result.diagnostic = Diagnostic(
        kAccessDenied, "sblr.dml.plan_import_rows.insert_access_denied",
        "insert_authorization_refused");
    return result;
  }
  if (request.context.database_path.empty()) {
    result.diagnostic = Diagnostic(
        kTransactionInvalid,
        "sblr.dml.plan_import_rows.transaction_invalid",
        "active_transaction_inventory_identity_required");
    return result;
  }
  auto transaction_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  if (!ActiveTransactionIdentity(request.context)) {
    result.diagnostic = Diagnostic(
        kTransactionInvalid,
        "sblr.dml.plan_import_rows.transaction_invalid",
        "active_transaction_inventory_identity_required");
    return result;
  }
  const auto derived = ResolveLiveImportRowsAuthority(
      request.context, request.structural_occurrence_id,
      UuidText(request.row.carriers.descriptor.target_table_uuid),
      request.live_authority.executor_availability_generation);
  if (!derived.ok ||
      !LiveAuthorityEqual(request.live_authority, derived.live)) {
    result.diagnostic = Diagnostic(
        kAuthorityMismatch,
        "sblr.dml.plan_import_rows.binder_authority_mismatch",
        derived.detail.empty() ? "live_engine_authority_projection_mismatch"
                               : derived.detail);
    return result;
  }
  codec_diagnostic = {};
  if (!codec::ValidatePlanImportRowsLiveAuthorityV1(
          request.row.carriers.descriptor, derived.live,
          &codec_diagnostic)) {
    result.diagnostic = CodecDiagnostic(
        codec_diagnostic, "binder_live_authority", kAuthorityMismatch);
    return result;
  }
  std::uint64_t current_availability_generation = 0;
  if (!LoadCurrentPlanImportRowsAvailability(
          request.context, &current_availability_generation) ||
      current_availability_generation !=
          derived.live.executor_availability_generation) {
    result.diagnostic = Diagnostic(
        kExecutorEvidenceMissing,
        "sblr.dml.plan_import_rows.executor_evidence_missing",
        "current_plan_import_executor_availability_required");
    return result;
  }

  codec::PlanImportRowsDescriptorRefV1 descriptor_ref;
  descriptor_ref.descriptor_uuid =
      request.row.carriers.descriptor.descriptor_uuid;
  descriptor_ref.descriptor_generation =
      request.row.carriers.descriptor.descriptor_generation;
  std::vector<std::uint8_t> encoded_ref;
  codec_diagnostic = {};
  if (!codec::EncodePlanImportRowsDescriptorRefV1(
          descriptor_ref, &encoded_ref, &codec_diagnostic) ||
      encoded_ref.size() != codec::kPlanImportRowsDescriptorRefBytesV1) {
    result.diagnostic =
        CodecDiagnostic(codec_diagnostic, "binder_descriptor_reference");
    return result;
  }

  const auto key = MakeRegistryKey(
      request.live_authority.authenticated_statement_receipt_uuid,
      request.structural_occurrence_id, descriptor_ref);
  const auto head_key = MakeRegistryHeadKey(
      request.live_authority.authenticated_statement_receipt_uuid,
      request.structural_occurrence_id, descriptor_ref.descriptor_uuid);
  const RegistryPrimaryKey primary_key{descriptor_ref.descriptor_uuid,
                                       descriptor_ref.descriptor_generation};
  auto& registry = BoundRegistry();
  std::unique_lock lock(registry.mutex);
  if (registry.rows.find(key) != registry.rows.end() ||
      registry.current_heads.find(head_key) != registry.current_heads.end() ||
      registry.primary_rows.find(primary_key) !=
          registry.primary_rows.end()) {
    result.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_row_immutable",
        "immutable_descriptor_head_already_published");
    return result;
  }
  registry.rows.emplace(key, request.row);
  registry.current_heads.emplace(head_key,
                                 descriptor_ref.descriptor_generation);
  registry.primary_rows.emplace(primary_key, key);

  result.ok = true;
  result.descriptor_ref = descriptor_ref;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineBindImportRowsPlanDescriptorResultV1
CreateAndPublishEngineBoundImportRowsPlanDescriptorV1(
    const EngineCreateImportRowsPlanDescriptorRequestV1& request) {
  EngineBindImportRowsPlanDescriptorResultV1 failure;
  if (!BinderContextAdmitted(request.context)) {
    failure.diagnostic = Diagnostic(
        kAccessDenied, "sblr.dml.plan_import_rows.binder_access_denied",
        "engine_import_binder_authority_required");
    return failure;
  }

  codec::PlanImportRowsUuidV1 target_table_uuid{};
  if (request.structural_occurrence_id == 0 ||
      !CanonicalUuidBytes(request.target_table_uuid.canonical,
                          &target_table_uuid) ||
      !MappingDemandShapeValid(request.mappings)) {
    failure.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_demand_invalid",
        "exact_target_table_and_structural_occurrence_required");
    return failure;
  }
  if (!HasInsertAuthorization(request.context, target_table_uuid)) {
    failure.diagnostic = Diagnostic(
        kAccessDenied, "sblr.dml.plan_import_rows.insert_access_denied",
        "insert_authorization_refused");
    return failure;
  }
  if (request.context.database_path.empty()) {
    failure.diagnostic = Diagnostic(
        kTransactionInvalid,
        "sblr.dml.plan_import_rows.transaction_invalid",
        "active_transaction_inventory_identity_required");
    return failure;
  }
  auto transaction_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  if (!ActiveTransactionIdentity(request.context)) {
    failure.diagnostic = Diagnostic(
        kTransactionInvalid,
        "sblr.dml.plan_import_rows.transaction_invalid",
        "active_transaction_inventory_identity_required");
    return failure;
  }
  auto derived = ResolveLiveImportRowsAuthority(
      request.context, request.structural_occurrence_id,
      request.target_table_uuid.canonical, 1);
  if (!derived.ok) {
    failure.diagnostic = Diagnostic(
        kAuthorityMismatch,
        "sblr.dml.plan_import_rows.binder_authority_mismatch",
        derived.detail.empty() ? "live_engine_authority_projection_mismatch"
                               : derived.detail);
    return failure;
  }
  if (!LoadCurrentPlanImportRowsAvailability(
          request.context,
          &derived.live.executor_availability_generation)) {
    failure.diagnostic = Diagnostic(
        kExecutorEvidenceMissing,
        "sblr.dml.plan_import_rows.executor_evidence_missing",
        "current_plan_import_executor_availability_required");
    return failure;
  }
  if (!request.mappings.empty()) {
    failure.diagnostic = Diagnostic(
        kUnsupported, "sblr.dml.plan_import_rows.operation_unsupported",
        "numeric_import_mapping_codec_projection_not_admitted");
    return failure;
  }

  codec::PlanImportRowsCarrierSetV1 carriers;
  auto& descriptor = carriers.descriptor;
  codec::PlanImportRowsUuidV1 source_binding_uuid{};
  if (!IssueBinderUuid(&descriptor.descriptor_uuid) ||
      !IssueBinderUuid(&descriptor.request_uuid) ||
      !IssueBinderUuid(&carriers.source.header.row_uuid) ||
      !IssueBinderUuid(&carriers.format.header.row_uuid) ||
      !IssueBinderUuid(&carriers.mapping.header.row_uuid) ||
      !IssueBinderUuid(&carriers.policy.header.row_uuid) ||
      !IssueBinderUuid(&source_binding_uuid)) {
    failure.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_identity_invalid",
        "engine_owned_import_identity_issuance_failed");
    return failure;
  }
  descriptor.descriptor_generation = 1;
  descriptor.authenticated_statement_receipt_uuid =
      derived.live.authenticated_statement_receipt_uuid;
  descriptor.structural_occurrence_id = request.structural_occurrence_id;
  descriptor.local_transaction_id = derived.live.local_transaction_id;
  descriptor.transaction_uuid = derived.live.transaction_uuid;
  descriptor.mga_snapshot_uuid = derived.live.mga_snapshot_uuid;
  descriptor.mga_snapshot_generation =
      derived.live.mga_snapshot_generation;
  descriptor.security_snapshot_uuid =
      derived.live.security_snapshot_uuid;
  descriptor.security_snapshot_generation =
      derived.live.security_snapshot_generation;
  descriptor.policy_snapshot_uuid =
      derived.live.policy_snapshot_uuid;
  descriptor.policy_snapshot_generation =
      derived.live.policy_snapshot_generation;
  descriptor.resource_admission_uuid =
      derived.live.resource_admission_uuid;
  descriptor.resource_admission_generation =
      derived.live.resource_admission_generation;
  descriptor.catalog_generation = derived.live.catalog_generation;
  descriptor.target_table_uuid = derived.live.target_table_uuid;
  descriptor.target_relation_descriptor_uuid =
      derived.live.target_relation_descriptor_uuid;
  descriptor.target_relation_descriptor_generation =
      derived.live.target_relation_descriptor_generation;
  descriptor.executor_availability_generation =
      derived.live.executor_availability_generation;

  const auto bind_child_owner = [&](codec::PlanImportRowsNestedHeaderV1* header) {
    header->row_generation = 1;
    header->owner_plan_descriptor_uuid = descriptor.descriptor_uuid;
    header->owner_plan_descriptor_generation = descriptor.descriptor_generation;
  };
  bind_child_owner(&carriers.source.header);
  bind_child_owner(&carriers.format.header);
  bind_child_owner(&carriers.mapping.header);
  bind_child_owner(&carriers.policy.header);

  carriers.source.source_kind = request.source_kind;
  carriers.source.flags = request.source_fingerprint_present
                              ? codec::kPlanImportRowsSourceFingerprintPresentV1
                              : 0;
  carriers.source.engine_source_binding_uuid = source_binding_uuid;
  carriers.source.engine_source_binding_generation = 1;
  carriers.source.source_fingerprint_sha256 =
      request.source_fingerprint_sha256;
  carriers.format.format_family = request.format_family;
  carriers.mapping.mappings.clear();
  carriers.policy.reject_mode = request.reject_mode;
  carriers.policy.reject_payload_policy = request.reject_payload_policy;
  carriers.policy.resume_policy = request.resume_policy;
  carriers.policy.flags = 0;
  if (request.strict_bulk_load_requested) {
    carriers.policy.flags |=
        codec::kPlanImportRowsStrictBulkLoadRequestedV1;
  }
  if (request.reference_relaxed_semantics_requested) {
    carriers.policy.flags |=
        codec::kPlanImportRowsReferenceRelaxedSemanticsRequestedV1;
  }
  carriers.policy.reject_limit_ppm = request.reject_limit_ppm;
  carriers.policy.reject_limit_rows = request.reject_limit_rows;
  carriers.policy.reject_target_relation_uuid =
      request.reject_target_relation_uuid;
  carriers.policy.reject_target_relation_generation =
      request.reject_target_relation_generation;
  carriers.policy.reject_target_relation_sha256 =
      request.reject_target_relation_sha256;

  codec::PlanImportRowsCodecDiagnosticV1 codec_diagnostic;
  if (!FreezeSource(&carriers.source, &codec_diagnostic) ||
      !FreezeFormat(&carriers.format, &codec_diagnostic) ||
      !FreezeMapping(&carriers.mapping, &codec_diagnostic) ||
      !FreezePolicy(&carriers.policy, &codec_diagnostic)) {
    failure.diagnostic =
        CodecDiagnostic(codec_diagnostic, "binder_semantic_demand");
    return failure;
  }
  descriptor.import_source_envelope_ref =
      ChildReference(carriers.source.header);
  descriptor.import_format_envelope_ref =
      ChildReference(carriers.format.header);
  descriptor.column_mapping_vector_ref =
      ChildReference(carriers.mapping.header);
  descriptor.import_policy_ref = ChildReference(carriers.policy.header);
  if (!FreezeDescriptor(&descriptor, &codec_diagnostic)) {
    failure.diagnostic =
        CodecDiagnostic(codec_diagnostic, "binder_plan_descriptor");
    return failure;
  }

  EngineBindImportRowsPlanDescriptorRequestV1 publish;
  publish.context = request.context;
  publish.structural_occurrence_id = request.structural_occurrence_id;
  publish.live_authority = derived.live;
  publish.row.carriers = std::move(carriers);
  publish.row.reference_relaxed_semantics_authorized =
      request.reference_relaxed_semantics_authorized;
  if (!IssueBinderUuid(&publish.row.executor_evidence_uuid)) {
    failure.diagnostic = Diagnostic(
        kOperandInvalid, "sblr.dml.plan_import_rows.binder_identity_invalid",
        "engine_owned_evidence_identity_issuance_failed");
    return failure;
  }
  publish.row.executor_evidence_generation = 1;
  return PublishEngineBoundImportRowsPlanDescriptorV1(publish);
}

EngineReleaseImportRowsPlanDescriptorsResultV1
ReleaseEngineBoundImportRowsPlanDescriptorsV1(
    const EngineRequestContext& context) {
  EngineReleaseImportRowsPlanDescriptorsResultV1 result;
  if (!BinderContextAdmitted(context)) {
    result.diagnostic = Diagnostic(
        kAccessDenied, "sblr.dml.plan_import_rows.release_access_denied",
        "engine_import_binder_authority_required");
    return result;
  }
  codec::PlanImportRowsUuidV1 receipt_uuid{};
  if (!CanonicalUuidBytes(context.statement_receipt_uuid.canonical,
                          &receipt_uuid)) {
    result.diagnostic = Diagnostic(
        kAuthorityMismatch,
        "sblr.dml.plan_import_rows.release_authority_mismatch",
        "authenticated_statement_receipt_mismatch");
    return result;
  }

  auto& registry = BoundRegistry();
  std::unique_lock lock(registry.mutex);
  for (auto it = registry.rows.begin(); it != registry.rows.end();) {
    if (std::get<0>(it->first) != receipt_uuid) {
      ++it;
      continue;
    }
    const auto head_key = MakeRegistryHeadKey(
        std::get<0>(it->first), std::get<1>(it->first),
        std::get<2>(it->first));
    const RegistryPrimaryKey primary_key{std::get<2>(it->first),
                                         std::get<3>(it->first)};
    registry.current_heads.erase(head_key);
    registry.primary_rows.erase(primary_key);
    it = registry.rows.erase(it);
    ++result.released_row_count;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EnginePlanImportRowsResult EnginePlanImportRows(
    const EnginePlanImportRowsRequest& request) {
  if (request.operation_id != kOperationId) {
    return ImportFailure(kOpcodeInvalid,
                         "sblr.dml.plan_import_rows.opcode_invalid",
                         "operation_identity_mismatch");
  }
  if (LegacyPlanningAuthorityPresent(request)) {
    return ImportFailure(kOperandInvalid,
                         "sblr.dml.plan_import_rows.operand_invalid",
                         "descriptor_reference_is_the_only_plan_operand");
  }

  codec::PlanImportRowsCodecDiagnosticV1 codec_diagnostic;
  std::vector<std::uint8_t> encoded_ref;
  if (!codec::EncodePlanImportRowsDescriptorRefV1(
          request.descriptor_ref, &encoded_ref, &codec_diagnostic) ||
      encoded_ref.size() != codec::kPlanImportRowsDescriptorRefBytesV1) {
    return ImportFailure(
        CodecDiagnostic(codec_diagnostic, "descriptor_reference"));
  }

  // Core separates cancellation observation from diagnostic emission. The
  // callback is sampled at every approved checkpoint and latched, while every
  // higher-precedence refusal through the cluster gate is still allowed to
  // win before PROCESS.CANCELLED can be emitted.
  bool cancellation_latched = false;
  const auto observe_cancellation = [&]() {
    if (CancellationObserved(request.context)) cancellation_latched = true;
  };

  // observe_before_descriptor_lookup
  observe_cancellation();

  auto& registry = BoundRegistry();
  std::shared_lock registry_lock(registry.mutex);
  const RegistryPrimaryKey primary_key{
      request.descriptor_ref.descriptor_uuid,
      request.descriptor_ref.descriptor_generation};
  const auto primary = registry.primary_rows.find(primary_key);
  if (primary == registry.primary_rows.end()) {
    return ImportFailure(kOperandInvalid,
                         "sblr.dml.plan_import_rows.operand_invalid",
                         "bound_descriptor_generation_not_found");
  }
  const auto& key = primary->second;
  const auto& bound_receipt_uuid = std::get<0>(key);
  const auto found = registry.rows.find(key);
  const auto structural_occurrence_id = std::get<1>(key);
  const auto head_key = MakeRegistryHeadKey(
      bound_receipt_uuid, structural_occurrence_id,
      request.descriptor_ref.descriptor_uuid);
  const auto head = registry.current_heads.find(head_key);
  if (found == registry.rows.end() || head == registry.current_heads.end() ||
      head->second != request.descriptor_ref.descriptor_generation) {
    return ImportFailure(kOperandInvalid,
                         "sblr.dml.plan_import_rows.operand_invalid",
                         "exact_bound_descriptor_head_not_found");
  }
  const auto& bound = found->second;
  const auto& carriers = bound.carriers;
  if (structural_occurrence_id == 0 ||
      carriers.descriptor.structural_occurrence_id !=
          structural_occurrence_id ||
      carriers.descriptor.authenticated_statement_receipt_uuid !=
          bound_receipt_uuid) {
    return ImportFailure(kOperandInvalid,
                         "sblr.dml.plan_import_rows.operand_invalid",
                         "bound_descriptor_registry_key_mismatch");
  }

  // observe_before_target_and_snapshot_binding
  observe_cancellation();

  codec_diagnostic = {};
  if (!codec::ValidatePlanImportRowsDescriptorReferenceV1(
          request.descriptor_ref, carriers.descriptor, &codec_diagnostic)) {
    return ImportFailure(
        CodecDiagnostic(codec_diagnostic, "descriptor_reference_binding"));
  }

  // observe_before_security_policy_and_resource_validation
  observe_cancellation();

  // observe_before_source_format_mapping_and_policy_validation
  observe_cancellation();

  codec_diagnostic = {};
  const bool carrier_admitted = codec::ValidatePlanImportRowsCarrierSetV1(
      carriers, &codec_diagnostic);
  if (!carrier_admitted && codec_diagnostic.code != kUnsupported) {
    return ImportFailure(
        CodecDiagnostic(codec_diagnostic, "carrier_validation"));
  }
  const bool unsupported_source_format =
      !carrier_admitted && codec_diagnostic.code == kUnsupported;

  codec_diagnostic = {};
  const bool policy_admitted = codec::ValidatePlanImportRowsPolicyAdmissionV1(
      carriers.policy, bound.reference_relaxed_semantics_authorized,
      &codec_diagnostic);
  if (!policy_admitted && codec_diagnostic.code != kUnsupported) {
    return ImportFailure(
        CodecDiagnostic(codec_diagnostic, "policy_validation"));
  }
  const bool unsupported_policy =
      !policy_admitted && codec_diagnostic.code == kUnsupported;

  if (!ConsumerContextAdmitted(request.context) ||
      !HasInsertAuthorization(request.context,
                              carriers.descriptor.target_table_uuid)) {
    return ImportFailure(kAccessDenied,
                         "sblr.dml.plan_import_rows.insert_access_denied",
                         "insert_authorization_refused");
  }

  if (request.context.database_path.empty()) {
    return ImportFailure(kTransactionInvalid,
                         "sblr.dml.plan_import_rows.transaction_invalid",
                         "active_transaction_inventory_identity_required");
  }
  auto transaction_guard =
      AcquireTransactionInventoryGuard(request.context.database_path);
  if (!ActiveTransactionIdentity(request.context)) {
    return ImportFailure(kTransactionInvalid,
                         "sblr.dml.plan_import_rows.transaction_invalid",
                         "active_transaction_inventory_identity_required");
  }

  codec::PlanImportRowsUuidV1 receipt_uuid{};
  if (!CanonicalUuidBytes(request.context.statement_receipt_uuid.canonical,
                          &receipt_uuid) ||
      receipt_uuid != bound_receipt_uuid) {
    return ImportFailure(kAuthorityMismatch,
                         "sblr.dml.plan_import_rows.authority_mismatch",
                         "authenticated_statement_receipt_mismatch");
  }

  auto derived = ResolveLiveImportRowsAuthority(
      request.context, structural_occurrence_id,
      UuidText(carriers.descriptor.target_table_uuid),
      carriers.descriptor.executor_availability_generation);
  if (!derived.ok) {
    return ImportFailure(kAuthorityMismatch,
                         "sblr.dml.plan_import_rows.authority_mismatch",
                         derived.detail.empty()
                             ? "live_engine_authority_projection_mismatch"
                             : derived.detail);
  }
  codec_diagnostic = {};
  if (!codec::ValidatePlanImportRowsLiveAuthorityV1(
          carriers.descriptor, derived.live, &codec_diagnostic)) {
    return ImportFailure(CodecDiagnostic(
        codec_diagnostic, "live_authority", kAuthorityMismatch));
  }

  if (!ClusterRouteAdmitted(request)) {
    return ImportFailure(
        kClusterFallthrough,
        "sblr.dml.plan_import_rows.cluster_fallthrough_forbidden",
        "authenticated_cluster_read_route_required");
  }
  if (cancellation_latched) {
    return ImportFailure(kCancelled,
                         "sblr.dml.plan_import_rows.cancelled",
                         "planning_cancelled_during_read_only_validation");
  }

  // observe_before_executor_evidence
  observe_cancellation();
  if (cancellation_latched) {
    return ImportFailure(kCancelled,
                         "sblr.dml.plan_import_rows.cancelled",
                         "planning_cancelled_before_evidence");
  }

  std::uint64_t current_availability_generation = 0;
  const bool executor_evidence_available =
      NonzeroBytes(bound.executor_evidence_uuid) &&
      bound.executor_evidence_generation != 0 &&
      LoadCurrentPlanImportRowsAvailability(
          request.context, &current_availability_generation) &&
      current_availability_generation ==
          carriers.descriptor.executor_availability_generation;
  if (!executor_evidence_available) {
    return ImportFailure(
        kExecutorEvidenceMissing,
        "sblr.dml.plan_import_rows.executor_evidence_missing",
        "accepted_executor_evidence_identity_required");
  }

  codec::PlanImportRowsExecutorEvidenceV1 evidence;
  evidence.evidence_uuid = bound.executor_evidence_uuid;
  evidence.evidence_generation = bound.executor_evidence_generation;
  evidence.request_descriptor_uuid = carriers.descriptor.descriptor_uuid;
  evidence.request_descriptor_generation =
      carriers.descriptor.descriptor_generation;
  evidence.request_projection_sha256 =
      carriers.descriptor.descriptor_evidence_sha256;
  evidence.executor_availability_generation =
      derived.live.executor_availability_generation;
  evidence.transaction_uuid = derived.live.transaction_uuid;
  evidence.local_transaction_id = derived.live.local_transaction_id;
  evidence.mga_snapshot_uuid = derived.live.mga_snapshot_uuid;
  evidence.mga_snapshot_generation =
      derived.live.mga_snapshot_generation;
  evidence.completed_validation_bits =
      codec::kPlanImportRowsAcceptedValidationBitsV1;

  std::vector<std::uint8_t> encoded_evidence;
  codec_diagnostic = {};
  const bool evidence_encoded =
      codec::EncodePlanImportRowsExecutorEvidenceV1(
          evidence, &encoded_evidence, &codec_diagnostic) &&
      encoded_evidence.size() ==
          codec::kPlanImportRowsExecutorEvidenceBytesV1;
  codec::PlanImportRowsExecutorEvidenceV1 accepted_evidence;
  bool evidence_accepted = false;
  if (evidence_encoded) {
    codec_diagnostic = {};
    evidence_accepted = codec::DecodePlanImportRowsExecutorEvidenceV1(
                            encoded_evidence.data(), encoded_evidence.size(),
                            &accepted_evidence, &codec_diagnostic) &&
                        codec::ValidatePlanImportRowsExecutorEvidenceBindingV1(
                            accepted_evidence, carriers.descriptor,
                            &codec_diagnostic);
  }
  if (!evidence_accepted) {
    return ImportFailure(
        kExecutorEvidenceMissing,
        "sblr.dml.plan_import_rows.executor_evidence_missing",
        "executor_evidence_binding_refused");
  }
  if (unsupported_source_format || unsupported_policy) {
    return ImportFailure(kUnsupported,
                         "sblr.dml.plan_import_rows.operation_unsupported",
                         "recognized_import_profile_not_admitted");
  }

  // observe_before_result_publication
  observe_cancellation();
  if (cancellation_latched) {
    return ImportFailure(kCancelled,
                         "sblr.dml.plan_import_rows.cancelled",
                         "planning_cancelled_before_result_publication");
  }
  const auto insert_mode = codec::NormalizePlanImportRowsInsertModeV1(
      carriers.source.source_kind);
  EnginePlanImportRowsResult result;
  result.ok = true;
  result.operation_id = std::string(kOperationId);
  result.surface_accepted = true;
  result.planning_only = true;
  result.execution_requires_execute_import_rows = true;
  result.row_execution_completed = false;
  result.row_persistence_claimed = false;
  result.normalized_insert_mode_code =
      static_cast<std::uint16_t>(insert_mode);
  result.normalized_source_kind_code =
      static_cast<std::uint16_t>(carriers.source.source_kind);
  result.normalized_format_family_code =
      static_cast<std::uint16_t>(carriers.format.format_family);
  result.normalized_insert_mode = InsertModeText(insert_mode);
  result.normalized_source_kind = SourceKindText(carriers.source.source_kind);
  result.normalized_format_family =
      FormatFamilyText(carriers.format.format_family);
  result.mapped_column_count =
      static_cast<EngineApiU64>(carriers.mapping.mappings.size());
  result.validated_request_descriptor_uuid.canonical =
      UuidText(carriers.descriptor.descriptor_uuid);
  result.validated_request_descriptor_generation =
      carriers.descriptor.descriptor_generation;
  result.validated_request_projection_sha256 =
      carriers.descriptor.descriptor_evidence_sha256;
  result.accepted_executor_evidence = std::move(accepted_evidence);
  result.transaction_uuid.canonical =
      UuidText(carriers.descriptor.transaction_uuid);
  result.local_transaction_id = carriers.descriptor.local_transaction_id;
  result.evidence.push_back(
      {"accepted_executor_evidence",
       UuidText(result.accepted_executor_evidence.evidence_uuid) + "@" +
           std::to_string(result.accepted_executor_evidence.evidence_generation) +
           "#sha256:" +
           Hex(result.accepted_executor_evidence.evidence_sha256)});
  return result;
}

}  // namespace scratchbird::engine::internal_api
