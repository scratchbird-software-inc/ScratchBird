// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/relation_descriptor_projection.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "crud_support/crud_store.hpp"
#include "dml/select_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOperation = "catalog.relation_descriptor_projection";

struct ProjectionViewDefinition {
  std::string variant;
  std::string source_relation_name;
  std::string function_uuid;
};

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const auto lhs = static_cast<unsigned char>(left[i]);
    const auto rhs = static_cast<unsigned char>(right[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) return false;
  }
  return true;
}

std::vector<std::string> OptionValues(const EngineApiRequest& request,
                                      const std::string& prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      values.push_back(option.substr(prefix.size()));
    }
  }
  return values;
}

std::string SingleOptionValue(const EngineApiRequest& request,
                              const std::string& prefix) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 ? values.front() : std::string{};
}

bool HasExactlyOneOption(const EngineApiRequest& request,
                         const std::string& prefix,
                         const std::string& expected) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 && values.front() == expected;
}

bool HasRawSqlOption(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    const std::string lowered = LowerAscii(option);
    if (lowered.rfind("sql_text:", 0) == 0 ||
        lowered.rfind("raw_sql:", 0) == 0 ||
        lowered.rfind("select_sql:", 0) == 0 ||
        lowered.rfind("view_sql:", 0) == 0 ||
        lowered.rfind("query_sql:", 0) == 0) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ProjectionDiagnostic(std::string detail) {
  return MakeInvalidRequestDiagnostic(kOperation, std::move(detail));
}

EngineApiDiagnostic ValidateExactReadableTransaction(
    const EngineRequestContext& context) {
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    return ProjectionDiagnostic("transaction_uuid_invalid");
  }
  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          context.database_path);
  if (!inventory.ok()) {
    return MakeEngineApiDiagnostic(
        inventory.diagnostic.diagnostic_code.empty()
            ? "SB-MGA-TXN-INV-LOAD-FAILED"
            : inventory.diagnostic.diagnostic_code,
        inventory.diagnostic.message_key.empty()
            ? "mga.transaction_inventory.load_failed"
            : inventory.diagnostic.message_key,
        inventory.diagnostic.remediation_hint,
        true);
  }
  const auto exact = scratchbird::transaction::mga::LookupLocalTransaction(
      inventory.inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(
          context.local_transaction_id));
  using scratchbird::transaction::mga::TransactionState;
  if (!exact.ok() ||
      exact.entry.identity.transaction_uuid.value !=
          parsed_transaction.value.value ||
      (exact.entry.state != TransactionState::active &&
       exact.entry.state != TransactionState::read_only_active)) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  return OkDiagnostic();
}

template <typename TResult>
TResult ProjectionFailure(const EngineRequestContext& context,
                          EngineApiDiagnostic diagnostic) {
  return MakeCrudDiagnosticResult<TResult>(
      context, "dml.select_rows", std::move(diagnostic));
}

bool IsSupportedVariant(const std::string& variant) {
  return variant == kRelationDescriptorProjectionTypeInventoryVariantV1 ||
         variant == kRelationDescriptorProjectionCharsetInventoryVariantV1;
}

bool IsSafePersistedCanonicalName(const std::string& value) {
  if (value.empty() || value.size() > 1024 ||
      value.find(';') != std::string::npos) {
    return false;
  }
  for (const unsigned char ch : value) {
    if (ch == 0 || ch == '\n' || ch == '\r' || ch == '\t') return false;
  }
  return true;
}

std::optional<std::uint64_t> ParseU64Strict(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

std::string PayloadFieldValue(const std::string& payload,
                              const std::string& prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end =
        delimiter == std::string::npos ? payload.size() : delimiter;
    const std::string field = payload.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      return field.substr(prefix.size());
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

std::vector<std::string> PayloadOptions(const std::string& payload) {
  const std::string prefix = "options=";
  std::size_t options_offset = 0;
  if (payload.rfind(prefix, 0) == 0) {
    options_offset = prefix.size();
  } else {
    const auto found = payload.find(";" + prefix);
    if (found == std::string::npos) return {};
    options_offset = found + 1 + prefix.size();
  }

  std::vector<std::string> options;
  std::size_t offset = options_offset;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end =
        delimiter == std::string::npos ? payload.size() : delimiter;
    const std::string option = payload.substr(offset, end - offset);
    if (!option.empty()) options.push_back(option);
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return options;
}

std::optional<ProjectionViewDefinition> ParseCanonicalViewOptions(
    const std::vector<std::string>& options) {
  if (options.size() != 4 && options.size() != 5) return std::nullopt;
  if (options[0] != std::string("view_query_shape:") +
                        kRelationDescriptorProjectionMarkerV1 ||
      options[1].rfind("view_source_name:", 0) != 0) {
    return std::nullopt;
  }

  ProjectionViewDefinition definition;
  definition.source_relation_name =
      options[1].substr(std::string("view_source_name:").size());
  if (!IsSafePersistedCanonicalName(definition.source_relation_name)) {
    return std::nullopt;
  }
  if (options[2] == "view_projection_count:1" &&
      options[3] ==
          std::string("view_projection_0:variant:") +
              kRelationDescriptorProjectionCharsetInventoryVariantV1 &&
      options.size() == 4) {
    definition.variant =
        kRelationDescriptorProjectionCharsetInventoryVariantV1;
    return definition;
  }
  if (options[2] == "view_projection_count:2" &&
      options[3] ==
          std::string("view_projection_0:variant:") +
              kRelationDescriptorProjectionTypeInventoryVariantV1 &&
      options.size() == 5 &&
      options[4].rfind("view_projection_1:function_uuid:", 0) == 0) {
    definition.variant = kRelationDescriptorProjectionTypeInventoryVariantV1;
    definition.function_uuid = options[4].substr(
        std::string("view_projection_1:function_uuid:").size());
    if (definition.function_uuid.empty()) return std::nullopt;
    return definition;
  }
  return std::nullopt;
}

bool PayloadRequestsRelationDescriptorProjection(
    const std::vector<std::string>& options) {
  return std::find(
             options.begin(),
             options.end(),
             std::string("view_query_shape:") +
                 kRelationDescriptorProjectionMarkerV1) != options.end();
}

bool CreatorInsideMetadataBoundary(const EngineRequestContext& context,
                                   std::uint64_t creator_tx) {
  if (creator_tx == 0) return false;
  if (creator_tx == context.local_transaction_id) return true;
  if (context.statement_metadata_snapshot_engine_owned) {
    if (std::find(
            context
                .statement_metadata_snapshot_active_excluded_local_transaction_ids
                .begin(),
            context
                .statement_metadata_snapshot_active_excluded_local_transaction_ids
                .end(),
            creator_tx) !=
            context
                .statement_metadata_snapshot_active_excluded_local_transaction_ids
                .end() ||
        std::find(
            context
                .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids
                .begin(),
            context
                .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids
                .end(),
            creator_tx) !=
            context
                .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids
                .end()) {
      return false;
    }
    return context
                   .statement_metadata_snapshot_visible_through_local_transaction_id !=
               0 &&
           creator_tx <=
               context
                   .statement_metadata_snapshot_visible_through_local_transaction_id;
  }
  const std::uint64_t boundary =
      context.snapshot_visible_through_local_transaction_id != 0
          ? context.snapshot_visible_through_local_transaction_id
          : context.local_transaction_id;
  return boundary != 0 && creator_tx <= boundary;
}

bool NameEntryMatches(const NameRegistryEntry& entry,
                      const EngineRequestContext& context,
                      const std::string& object_uuid,
                      const std::string& object_class,
                      const std::string& schema_uuid,
                      const std::string& expected_name) {
  if (entry.object_uuid != object_uuid ||
      entry.object_class != object_class || entry.deleted ||
      entry.lifecycle_state != "active" ||
      !CreatorInsideMetadataBoundary(context, entry.creator_tx)) {
    return false;
  }
  if (!schema_uuid.empty() && entry.scope_uuid != schema_uuid &&
      entry.parent_schema_uuid != schema_uuid) {
    return false;
  }
  if (EqualsAsciiInsensitive(entry.raw_name_text, expected_name) ||
      EqualsAsciiInsensitive(entry.display_name, expected_name)) {
    return true;
  }
  const std::string profile =
      entry.identifier_profile_uuid.empty()
          ? NameRegistryDefaultIdentifierProfile(context)
          : entry.identifier_profile_uuid;
  return entry.normalized_lookup_key ==
         NameRegistryLookupKey(expected_name, profile, false);
}

EngineApiDiagnostic ValidateRegisteredName(const EngineRequestContext& context,
                                           const std::string& object_uuid,
                                           const std::string& object_class,
                                           const std::string& schema_uuid,
                                           const std::string& expected_name,
                                           const std::string& detail) {
  const auto loaded =
      LoadNameRegistryState(context, context.local_transaction_id);
  if (!loaded.ok) return loaded.diagnostic;
  for (const auto& entry : loaded.state.entries) {
    if (NameEntryMatches(entry,
                         context,
                         object_uuid,
                         object_class,
                         schema_uuid,
                         expected_name)) {
      return OkDiagnostic();
    }
  }
  return ProjectionDiagnostic(detail);
}

std::string ExecutablePayloadField(const std::string& payload,
                                   const std::string& prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end =
        delimiter == std::string::npos ? payload.size() : delimiter;
    const std::string field = payload.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      return field.substr(prefix.size());
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

EngineApiDiagnostic ValidateTypeNameFunction(
    const EngineRequestContext& context,
    const std::string& function_uuid,
    std::uint64_t latest_allowed_creator_tx) {
  if (function_uuid.empty()) {
    return ProjectionDiagnostic("type_name_function_uuid_required");
  }
  const auto loaded = LoadExecutableObjectLifecycleState(context);
  if (!loaded.ok) return loaded.diagnostic;

  const EngineExecutableObjectRecord* function = nullptr;
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid != function_uuid) continue;
    if (function == nullptr || object.event_sequence > function->event_sequence) {
      function = &object;
    }
  }
  if (function == nullptr || function->object_kind != "function" ||
      function->lifecycle_state != "active" || function->deleted ||
      function->invalidated || function->creator_tx == 0 ||
      !CreatorInsideMetadataBoundary(context, function->creator_tx) ||
      function->creator_tx > latest_allowed_creator_tx ||
      function->executor_kind != "metadata_only" ||
      function->side_effect_class != "none" ||
      !function->stored_sblr_hash.empty() ||
      !function->internal_procedure_id.empty() ||
      ExecutablePayloadField(function->payload,
                             "compiled_body_descriptor:") !=
          kRelationTypeNameFunctionDescriptorV1) {
    return ProjectionDiagnostic(
        "type_name_function_dependency_not_visible_or_invalid");
  }
  return OkDiagnostic();
}

EngineDescriptor ScalarDescriptor(const std::string& canonical_type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
  descriptor.encoded_descriptor = "canonical=" + canonical_type_name;
  return descriptor;
}

EngineTypedValue ScalarValue(const EngineDescriptor& descriptor,
                             std::string value) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

EngineTypedValue NullValue(const EngineDescriptor& descriptor) {
  EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value.clear();
  typed.is_null = true;
  typed.state = EngineValueState::sql_null;
  return typed;
}

std::string EncodedDescriptorField(const std::string& descriptor,
                                   const std::string& requested_field) {
  const std::string lowered_field = LowerAscii(requested_field);
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto delimiter = descriptor.find(';', offset);
    const auto end =
        delimiter == std::string::npos ? descriptor.size() : delimiter;
    const std::string field = descriptor.substr(offset, end - offset);
    const auto equals = field.find('=');
    if (equals != std::string::npos &&
        LowerAscii(field.substr(0, equals)) == lowered_field) {
      return field.substr(equals + 1);
    }
    if (delimiter == std::string::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

bool IsTextLargeObject(
    const MgaRelationColumnStorageDescriptor& column) {
  return LowerAscii(EncodedDescriptorField(
             column.value_descriptor.encoded_descriptor,
             "text_resource_storage")) == "large_object";
}

struct ResolvedColumnResources {
  std::string charset_name;
  std::string collation_name;
};

EngineApiDiagnostic ResolveColumnResources(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& column,
    ResolvedColumnResources* resources) {
  if (resources == nullptr) {
    return ProjectionDiagnostic("column_resource_output_required");
  }
  if (column.charset_uuid.empty() && !column.collation_uuid.empty()) {
    return ProjectionDiagnostic("collation_requires_charset");
  }
  if (!column.charset_uuid.empty()) {
    EngineUuid charset_uuid;
    charset_uuid.canonical = column.charset_uuid;
    const auto charset = LookupEngineResourceDescriptorByUuid(
        context, charset_uuid, "charset");
    if (!charset.ok) return charset.diagnostic;
    if (!charset.resource_descriptor.present ||
        charset.resource_descriptor.resource_uuid.canonical !=
            column.charset_uuid ||
        charset.resource_descriptor.canonical_name.empty()) {
      return ProjectionDiagnostic("charset_descriptor_invalid");
    }
    resources->charset_name =
        charset.resource_descriptor.canonical_name;
  }
  if (!column.collation_uuid.empty()) {
    EngineUuid collation_uuid;
    collation_uuid.canonical = column.collation_uuid;
    const auto collation = LookupEngineResourceDescriptorByUuid(
        context, collation_uuid, "collation");
    if (!collation.ok) return collation.diagnostic;
    if (!collation.resource_descriptor.present ||
        collation.resource_descriptor.resource_uuid.canonical !=
            column.collation_uuid ||
        collation.resource_descriptor.canonical_name.empty() ||
        collation.resource_descriptor.parent_resource_uuid.canonical !=
            column.charset_uuid) {
      return ProjectionDiagnostic("collation_descriptor_invalid");
    }
    resources->collation_name =
        collation.resource_descriptor.canonical_name;
  }
  return OkDiagnostic();
}

bool CanonicalRelationColumns(
    const MgaRelationStorageDescriptor& descriptor,
    std::vector<MgaRelationColumnStorageDescriptor>* columns) {
  if (columns == nullptr || descriptor.columns.empty()) return false;
  *columns = descriptor.columns;
  std::stable_sort(columns->begin(),
                   columns->end(),
                   [](const auto& left, const auto& right) {
                     return left.ordinal < right.ordinal;
                   });
  std::set<std::string> column_uuids;
  std::set<std::string> column_names;
  for (std::size_t i = 0; i < columns->size(); ++i) {
    const auto& column = (*columns)[i];
    if (column.ordinal != i || column.column_uuid.canonical.empty() ||
        column.canonical_name_key.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        column.value_descriptor.encoded_descriptor.empty() ||
        !column_uuids.insert(column.column_uuid.canonical).second ||
        !column_names.insert(LowerAscii(column.canonical_name_key)).second) {
      return false;
    }
  }
  return true;
}

bool RequestContainsOnlyOneMarker(const EngineApiRequest& request,
                                  const std::string& prefix,
                                  const std::string& value) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 && values.front() == value;
}

}  // namespace

bool IsRelationDescriptorProjectionViewCreateRequest(
    const EngineApiRequest& request) {
  return RequestContainsOnlyOneMarker(
      request,
      "view_query_shape:",
      kRelationDescriptorProjectionMarkerV1);
}

EngineApiDiagnostic ValidateRelationDescriptorProjectionViewCreate(
    const EngineApiRequest& request) {
  if (!IsRelationDescriptorProjectionViewCreateRequest(request)) {
    return ProjectionDiagnostic("projection_marker_required");
  }
  if (request.target_object.object_kind != "view" &&
      !request.target_object.object_kind.empty()) {
    return ProjectionDiagnostic("projection_target_must_be_view");
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.canonical.empty()) {
    return ProjectionDiagnostic("exact_active_transaction_identity_required");
  }
  const auto exact_selector =
      ValidateExecutableObjectExactMgaSelector(request.context);
  if (exact_selector.error) return exact_selector;
  if (HasRawSqlOption(request)) {
    return ProjectionDiagnostic("raw_sql_forbidden");
  }
  const std::string view_name = ApiBehaviorPrimaryName(request, {});
  const std::string source_name =
      SingleOptionValue(request, "view_source_name:");
  if (!IsSafePersistedCanonicalName(view_name)) {
    return ProjectionDiagnostic("projection_view_name_invalid");
  }
  if (!IsSafePersistedCanonicalName(source_name) ||
      OptionValues(request, "view_source_name:").size() != 1) {
    return ProjectionDiagnostic("projection_source_name_invalid");
  }

  const std::string projection_zero =
      SingleOptionValue(request, "view_projection_0:");
  ProjectionViewDefinition definition;
  if (projection_zero ==
      std::string("variant:") +
          kRelationDescriptorProjectionTypeInventoryVariantV1) {
    definition.variant = kRelationDescriptorProjectionTypeInventoryVariantV1;
    if (!HasExactlyOneOption(request, "view_projection_count:", "2")) {
      return ProjectionDiagnostic("type_projection_count_invalid");
    }
    const std::string dependency =
        SingleOptionValue(request, "view_projection_1:");
    if (dependency.rfind("function_uuid:", 0) != 0) {
      return ProjectionDiagnostic("type_name_function_uuid_required");
    }
    definition.function_uuid =
        dependency.substr(std::string("function_uuid:").size());
    if (definition.function_uuid.empty()) {
      return ProjectionDiagnostic("type_name_function_uuid_required");
    }
  } else if (projection_zero ==
             std::string("variant:") +
                 kRelationDescriptorProjectionCharsetInventoryVariantV1) {
    definition.variant =
        kRelationDescriptorProjectionCharsetInventoryVariantV1;
    if (!HasExactlyOneOption(request, "view_projection_count:", "1") ||
        !OptionValues(request, "view_projection_1:").empty()) {
      return ProjectionDiagnostic("charset_projection_shape_invalid");
    }
  } else {
    return ProjectionDiagnostic("projection_variant_invalid");
  }
  if (!IsSupportedVariant(definition.variant)) {
    return ProjectionDiagnostic("projection_variant_invalid");
  }

  if (definition.variant ==
      kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        request.context,
        definition.function_uuid,
        request.context.local_transaction_id);
    if (dependency.error) return dependency;
  }
  return OkDiagnostic();
}

std::vector<std::string> CanonicalRelationDescriptorProjectionViewOptions(
    const EngineApiRequest& request) {
  const std::string projection_zero =
      SingleOptionValue(request, "view_projection_0:");
  const std::string source_name =
      SingleOptionValue(request, "view_source_name:");
  std::vector<std::string> options = {
      std::string("view_query_shape:") +
          kRelationDescriptorProjectionMarkerV1,
      std::string("view_source_name:") + source_name};
  if (projection_zero ==
      std::string("variant:") +
          kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const std::string function_uuid =
        SingleOptionValue(request, "view_projection_1:")
            .substr(std::string("function_uuid:").size());
    options.push_back("view_projection_count:2");
    options.push_back(
        std::string("view_projection_0:variant:") +
        kRelationDescriptorProjectionTypeInventoryVariantV1);
    options.push_back("view_projection_1:function_uuid:" + function_uuid);
  } else {
    options.push_back("view_projection_count:1");
    options.push_back(
        std::string("view_projection_0:variant:") +
        kRelationDescriptorProjectionCharsetInventoryVariantV1);
  }
  return options;
}

EngineCatalogRelationProjectionViewDescriptor
DescribeEngineCatalogRelationProjectionView(
    const EngineRequestContext& context,
    const std::string& view_uuid) {
  EngineCatalogRelationProjectionViewDescriptor descriptor;
  descriptor.diagnostic = OkDiagnostic();
  if (view_uuid.empty()) {
    descriptor.diagnostic = ProjectionDiagnostic("projection_view_uuid_required");
    return descriptor;
  }
  const auto exact_transaction = ValidateExactReadableTransaction(context);
  if (exact_transaction.error) {
    descriptor.diagnostic = exact_transaction;
    return descriptor;
  }

  const auto view = FindVisibleApiBehaviorRecord(
      context, view_uuid, context.local_transaction_id);
  if (!view) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_not_visible");
    return descriptor;
  }
  const auto options = PayloadOptions(view->payload);
  if (!PayloadRequestsRelationDescriptorProjection(options)) {
    // An ordinary persisted view is not an error for this classifier.
    return descriptor;
  }
  if (view->object_kind != "view" ||
      view->operation_id != "ddl.create_view" || view->state != "created" ||
      view->deleted || view->creator_tx == 0 ||
      !CreatorInsideMetadataBoundary(context, view->creator_tx) ||
      !IsSafePersistedCanonicalName(view->default_name)) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_not_visible_or_invalid");
    return descriptor;
  }

  const auto definition = ParseCanonicalViewOptions(options);
  const std::string view_schema_uuid =
      PayloadFieldValue(view->payload, "schema=");
  if (!definition || view_schema_uuid.empty()) {
    descriptor.diagnostic =
        ProjectionDiagnostic("projection_view_definition_invalid");
    return descriptor;
  }
  const auto view_name = ValidateRegisteredName(context,
                                                view_uuid,
                                                "view",
                                                view_schema_uuid,
                                                view->default_name,
                                                "projection_view_name_not_visible");
  if (view_name.error) {
    descriptor.diagnostic = view_name;
    return descriptor;
  }
  if (definition->variant ==
      kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        context, definition->function_uuid, view->creator_tx);
    if (dependency.error) {
      descriptor.diagnostic = dependency;
      return descriptor;
    }
  } else if (!definition->function_uuid.empty()) {
    descriptor.diagnostic =
        ProjectionDiagnostic("charset_projection_function_forbidden");
    return descriptor;
  }

  descriptor.present = true;
  descriptor.semantic_variant = definition->variant;
  descriptor.source_relation_name = definition->source_relation_name;
  descriptor.function_uuid = definition->function_uuid;
  return descriptor;
}

bool IsRelationDescriptorProjectionSelectRequest(
    const EngineSelectRowsRequest& request) {
  return RequestContainsOnlyOneMarker(
             request,
             "source_kind:",
             kRelationDescriptorProjectionSourceKind) &&
         RequestContainsOnlyOneMarker(
             request,
             "result_projection:",
             kRelationDescriptorProjectionMarkerV1);
}

EngineSelectRowsResult EngineSelectRelationDescriptorProjection(
    const EngineSelectRowsRequest& request) {
  if (!IsRelationDescriptorProjectionSelectRequest(request)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("projection_dispatch_invalid"));
  }
  if (request.context.local_transaction_id == 0 ||
      request.context.transaction_uuid.canonical.empty()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("exact_active_transaction_identity_required"));
  }
  if (HasRawSqlOption(request)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("raw_sql_forbidden"));
  }

  const std::string variant =
      SingleOptionValue(request, "dml_surface_variant:");
  if (!IsSupportedVariant(variant)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, ProjectionDiagnostic("projection_variant_invalid"));
  }
  const std::string view_uuid = request.target_object.uuid.canonical;
  const std::string relation_uuid =
      SingleOptionValue(request, "source_uuid:");
  const std::string expected_descriptor_uuid =
      SingleOptionValue(request, "source_fingerprint:");
  const auto expected_descriptor_generation =
      ParseU64Strict(SingleOptionValue(request, "source_position:"));
  if (view_uuid.empty() || relation_uuid.empty() ||
      expected_descriptor_uuid.empty() ||
      !expected_descriptor_generation ||
      *expected_descriptor_generation == 0 || view_uuid == relation_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_identity_envelope_invalid"));
  }

  const auto view = FindVisibleApiBehaviorRecord(
      request.context, view_uuid, request.context.local_transaction_id);
  if (!view || view->object_kind != "view" ||
      view->operation_id != "ddl.create_view" || view->state != "created" ||
      view->deleted || view->creator_tx == 0 ||
      !CreatorInsideMetadataBoundary(request.context, view->creator_tx) ||
      !IsSafePersistedCanonicalName(view->default_name)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_view_not_visible_or_invalid"));
  }

  const auto persisted_definition =
      ParseCanonicalViewOptions(PayloadOptions(view->payload));
  if (!persisted_definition || persisted_definition->variant != variant ||
      PayloadFieldValue(view->payload, "schema=").empty()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_view_definition_invalid"));
  }
  const std::string view_schema_uuid =
      PayloadFieldValue(view->payload, "schema=");
  const auto view_name = ValidateRegisteredName(request.context,
                                                view_uuid,
                                                "view",
                                                view_schema_uuid,
                                                view->default_name,
                                                "projection_view_name_not_visible");
  if (view_name.error) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, view_name);
  }
  if (variant == kRelationDescriptorProjectionTypeInventoryVariantV1) {
    const auto dependency = ValidateTypeNameFunction(
        request.context,
        persisted_definition->function_uuid,
        view->creator_tx);
    if (dependency.error) {
      return ProjectionFailure<EngineSelectRowsResult>(
          request.context, dependency);
    }
  } else if (!persisted_definition->function_uuid.empty()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("charset_projection_function_forbidden"));
  }

  const auto loaded_descriptor =
      LoadMgaRelationStorageDescriptor(request.context, relation_uuid);
  if (!loaded_descriptor.ok) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, loaded_descriptor.diagnostic);
  }
  const auto& descriptor = loaded_descriptor.descriptor;
  if (descriptor.relation_uuid.canonical != relation_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_uuid_mismatch"));
  }
  if (descriptor.relation_kind != "table") {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_kind_mismatch"));
  }
  if (descriptor.descriptor_uuid.canonical != expected_descriptor_uuid) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_descriptor_uuid_mismatch"));
  }
  if (descriptor.descriptor_generation !=
      *expected_descriptor_generation) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_descriptor_generation_mismatch"));
  }
  if (descriptor.schema_uuid.canonical.empty()) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_schema_uuid_required"));
  }
  const auto source_name = ValidateRegisteredName(request.context,
                                                  relation_uuid,
                                                  "table",
                                                  descriptor.schema_uuid.canonical,
                                                  persisted_definition
                                                      ->source_relation_name,
                                                  "projection_source_name_not_visible");
  if (source_name.error) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context, source_name);
  }

  std::vector<MgaRelationColumnStorageDescriptor> columns;
  if (!CanonicalRelationColumns(descriptor, &columns)) {
    return ProjectionFailure<EngineSelectRowsResult>(
        request.context,
        ProjectionDiagnostic("projection_relation_columns_invalid"));
  }

  const EngineDescriptor uuid_descriptor = ScalarDescriptor("uuid");
  const EngineDescriptor text_descriptor = ScalarDescriptor("text");
  const EngineDescriptor ordinal_descriptor = ScalarDescriptor("uint64");
  const EngineDescriptor boolean_descriptor = ScalarDescriptor("boolean");

  std::vector<EngineRowValue> rows;
  rows.reserve(columns.size());
  for (const auto& column : columns) {
    ResolvedColumnResources resources;
    const auto resources_resolved =
        ResolveColumnResources(request.context, column, &resources);
    if (resources_resolved.error) {
      return ProjectionFailure<EngineSelectRowsResult>(
          request.context, resources_resolved);
    }

    EngineRowValue row;
    row.requested_row_uuid = column.column_uuid;
    row.fields.reserve(10);
    row.fields.push_back(
        {"column_uuid",
         ScalarValue(uuid_descriptor, column.column_uuid.canonical)});
    row.fields.push_back(
        {"canonical_name_key",
         ScalarValue(text_descriptor, column.canonical_name_key)});
    row.fields.push_back(
        {"ordinal",
         ScalarValue(ordinal_descriptor, std::to_string(column.ordinal))});
    row.fields.push_back(
        {"canonical_type_name",
         ScalarValue(text_descriptor,
                     column.value_descriptor.canonical_type_name)});
    row.fields.push_back(
        {"character_length",
         column.character_length == 0
             ? NullValue(ordinal_descriptor)
             : ScalarValue(ordinal_descriptor,
                           std::to_string(column.character_length))});
    row.fields.push_back(
        {"charset_uuid",
         column.charset_uuid.empty()
             ? NullValue(uuid_descriptor)
             : ScalarValue(uuid_descriptor, column.charset_uuid)});
    row.fields.push_back(
        {"charset_canonical_name",
         column.charset_uuid.empty()
             ? NullValue(text_descriptor)
             : ScalarValue(text_descriptor, resources.charset_name)});
    row.fields.push_back(
        {"collation_uuid",
         column.collation_uuid.empty()
             ? NullValue(uuid_descriptor)
             : ScalarValue(uuid_descriptor, column.collation_uuid)});
    row.fields.push_back(
        {"collation_canonical_name",
         column.collation_uuid.empty()
             ? NullValue(text_descriptor)
             : ScalarValue(text_descriptor, resources.collation_name)});
    row.fields.push_back(
        {"text_large_object",
         ScalarValue(boolean_descriptor,
                     IsTextLargeObject(column) ? "true" : "false")});
    rows.push_back(std::move(row));
  }

  auto result = MakeCrudSuccessResult<EngineSelectRowsResult>(
      request.context, "dml.select_rows");
  result.primary_object.uuid.canonical = view_uuid;
  result.primary_object.object_kind = "view";
  result.visible_count = rows.size();
  result.result_shape.result_kind =
      "catalog_relation_descriptor_projection";
  result.result_shape.columns = {uuid_descriptor,
                                 text_descriptor,
                                 ordinal_descriptor,
                                 text_descriptor,
                                 ordinal_descriptor,
                                 uuid_descriptor,
                                 text_descriptor,
                                 uuid_descriptor,
                                 text_descriptor,
                                 boolean_descriptor};
  result.result_shape.rows = std::move(rows);
  result.evidence.push_back(
      {"catalog_projection_marker",
       kRelationDescriptorProjectionMarkerV1});
  result.evidence.push_back(
      {"catalog_projection_view_uuid", view_uuid});
  result.evidence.push_back(
      {"catalog_projection_relation_uuid", relation_uuid});
  result.evidence.push_back(
      {"catalog_projection_descriptor_uuid",
       descriptor.descriptor_uuid.canonical});
  result.evidence.push_back(
      {"catalog_projection_descriptor_generation",
       std::to_string(descriptor.descriptor_generation)});
  result.evidence.push_back(
      {"catalog_projection_mga_authority", "durable_transaction_inventory"});
  result.evidence.push_back(
      {"catalog_projection_parser_sql", "false"});
  result.evidence.push_back({"catalog_projection_variant", variant});
  result.evidence.push_back(
      {"catalog_projection_source_name",
       persisted_definition->source_relation_name});
  result.evidence.push_back(
      {"catalog_projection_view_creator_transaction",
       std::to_string(view->creator_tx)});
  if (!persisted_definition->function_uuid.empty()) {
    result.evidence.push_back(
        {"catalog_projection_function_uuid",
         persisted_definition->function_uuid});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
