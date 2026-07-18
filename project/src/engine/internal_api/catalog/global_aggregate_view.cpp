// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/global_aggregate_view.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_operations.hpp"
#include "dml/global_aggregate_projection.hpp"
#include "dml/select_api.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
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

namespace dt = scratchbird::core::datatypes;

constexpr std::string_view kOperation = "catalog.global_aggregate_view";

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic ViewDiagnostic(std::string detail) {
  return MakeInvalidRequestDiagnostic(
      std::string(kOperation), std::move(detail));
}

bool DescriptorExactlyMatches(const EngineDescriptor& left,
                              const EngineDescriptor& right) {
  return left.descriptor_uuid.canonical ==
             right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

std::vector<std::string> OptionValues(const EngineApiRequest& request,
                                      std::string_view prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      values.push_back(option.substr(prefix.size()));
    }
  }
  return values;
}

std::string SingleOptionValue(const EngineApiRequest& request,
                              std::string_view prefix) {
  const auto values = OptionValues(request, prefix);
  return values.size() == 1 ? values.front() : std::string{};
}

std::optional<std::uint64_t> ParseCanonicalU64(std::string_view value) {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::int32_t> ParseCanonicalI32(std::string_view value) {
  if (value.empty() || value == "-0" || value.front() == '+' ||
      (value.size() > 1 && value.front() == '0') ||
      (value.size() > 2 && value[0] == '-' && value[1] == '0')) {
    return std::nullopt;
  }
  std::int32_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

bool SafeCanonicalIdentifier(std::string_view identifier) {
  if (identifier.empty() || identifier.size() > 128) return false;
  const auto ascii_alpha = [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
  };
  const auto ascii_digit = [](unsigned char ch) {
    return ch >= '0' && ch <= '9';
  };
  const unsigned char first =
      static_cast<unsigned char>(identifier.front());
  if (!ascii_alpha(first) && first != '_') return false;
  for (const unsigned char ch : identifier) {
    if (!ascii_alpha(ch) && !ascii_digit(ch) && ch != '_') return false;
  }
  return true;
}

bool SafeAlias(std::string_view alias) {
  return SafeCanonicalIdentifier(alias);
}

bool SafeViewName(std::string_view name) {
  return SafeCanonicalIdentifier(name);
}

bool CanonicalTypedUuid(scratchbird::core::platform::UuidKind kind,
                        std::string_view value) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
      kind, std::string(value));
  return parsed.ok() &&
         scratchbird::core::uuid::UuidToString(parsed.value.value) == value;
}

bool HasForbiddenRequestData(const EngineApiRequest& request) {
  if (!request.rows.empty() || !request.constraints.empty() ||
      !request.indexes.empty() ||
      !request.predicate.predicate_kind.empty() ||
      !request.predicate.canonical_predicate_envelope.empty() ||
      !request.predicate.bound_values.empty() ||
      !request.ordering.canonical_ordering_envelopes.empty() ||
      !request.physical_profile.names.empty() ||
      !request.physical_profile.encoded_profiles.empty() ||
      !request.policy_profile.names.empty() ||
      !request.policy_profile.encoded_profiles.empty() ||
      !request.compatibility_profile.names.empty() ||
      !request.compatibility_profile.encoded_profiles.empty() ||
      !request.diagnostic_options.empty()) {
    return true;
  }
  for (const auto& option : request.option_envelopes) {
    std::string lowered = option;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    if (lowered.rfind("sql_text:", 0) == 0 ||
        lowered.rfind("raw_sql:", 0) == 0 ||
        lowered.rfind("select_sql:", 0) == 0 ||
        lowered.rfind("view_sql:", 0) == 0 ||
        lowered.rfind("query_sql:", 0) == 0 ||
        lowered.find("parser") != std::string::npos ||
        lowered.find("dialect") != std::string::npos) {
      return true;
    }
  }
  return false;
}

EngineApiDiagnostic ValidateExactActiveTransaction(
    const EngineRequestContext& context,
    bool require_write) {
  if (context.database_path.empty() || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty()) {
    return ViewDiagnostic("exact_active_transaction_identity_required");
  }
  const auto parsed_transaction = scratchbird::core::uuid::ParseTypedUuid(
      scratchbird::core::platform::UuidKind::transaction,
      context.transaction_uuid.canonical);
  if (!parsed_transaction.ok()) {
    return ViewDiagnostic("transaction_uuid_invalid");
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
       exact.entry.state != TransactionState::read_only_active) ||
      (require_write &&
       (exact.entry.state == TransactionState::read_only_active ||
        context.read_only_mode))) {
    return ViewDiagnostic(require_write
                              ? "exact_active_write_transaction_required"
                              : "exact_active_transaction_identity_required");
  }
  return OkDiagnostic();
}

std::string PayloadFieldValue(std::string_view payload,
                              std::string_view prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end = delimiter == std::string_view::npos
                         ? payload.size()
                         : delimiter;
    const auto field = payload.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) {
      return std::string(field.substr(prefix.size()));
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  return {};
}

std::vector<std::string> PayloadOptions(std::string_view payload) {
  constexpr std::string_view prefix = "options=";
  std::size_t options_offset = 0;
  if (payload.rfind(prefix, 0) == 0) {
    options_offset = prefix.size();
  } else {
    const auto found = payload.find(";options=");
    if (found == std::string_view::npos) return {};
    options_offset = found + std::string_view(";options=").size();
  }
  std::vector<std::string> options;
  std::size_t offset = options_offset;
  while (offset <= payload.size()) {
    const auto delimiter = payload.find(';', offset);
    const auto end = delimiter == std::string_view::npos
                         ? payload.size()
                         : delimiter;
    if (end != offset) {
      options.emplace_back(payload.substr(offset, end - offset));
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  return options;
}

bool PayloadRequestsBoundedView(const std::vector<std::string>& options) {
  return std::count(options.begin(),
                    options.end(),
                    std::string("view_query_shape:") +
                        kEngineGlobalAggregateViewMarkerV1) == 1;
}

bool PayloadMentionsBoundedViewFamily(
    const std::vector<std::string>& options) {
  return std::any_of(options.begin(), options.end(), [](const auto& option) {
    return option.rfind("view_query_shape:engine.global_aggregate_view", 0) ==
           0;
  });
}

std::optional<EngineGlobalAggregateViewDescriptor> ParsePersistedDescriptor(
    const ApiBehaviorRecord& record) {
  const auto options = PayloadOptions(record.payload);
  if (!PayloadRequestsBoundedView(options)) return std::nullopt;
  if (options.size() != 15 ||
      options[0] != std::string("view_query_shape:") +
                        kEngineGlobalAggregateViewMarkerV1 ||
      options[1].rfind("view_descriptor_uuid:", 0) != 0 ||
      options[2].rfind("view_descriptor_generation:", 0) != 0 ||
      options[3].rfind("source_relation_uuid:", 0) != 0 ||
      options[4].rfind("source_relation_descriptor_uuid:", 0) != 0 ||
      options[5].rfind("source_relation_descriptor_generation:", 0) != 0 ||
      options[6].rfind("source_column_uuid:", 0) != 0 ||
      options[7].rfind("source_column_descriptor_uuid:", 0) != 0 ||
      options[8] != std::string("expression_kind:") +
                        kEngineGlobalAggregateViewInt32MultiplyV1 ||
      options[9] != "expression_literal_type:int32" ||
      options[10].rfind("expression_literal_value:", 0) != 0 ||
      options[11] != "expression_result_type:int64" ||
      options[12].rfind("aggregate_function_uuid:", 0) != 0 ||
      options[13].rfind("aggregate_result_alias:", 0) != 0 ||
      options[14] != "aggregate_result_type:int64_nullable") {
    return std::nullopt;
  }

  EngineGlobalAggregateViewDescriptor descriptor;
  descriptor.view_uuid.canonical = record.object_uuid;
  descriptor.view_descriptor_uuid.canonical =
      options[1].substr(std::string("view_descriptor_uuid:").size());
  const auto view_generation = ParseCanonicalU64(
      options[2].substr(
          std::string("view_descriptor_generation:").size()));
  descriptor.source_relation_uuid.canonical =
      options[3].substr(std::string("source_relation_uuid:").size());
  descriptor.source_relation_descriptor_uuid.canonical =
      options[4].substr(
          std::string("source_relation_descriptor_uuid:").size());
  const auto source_generation = ParseCanonicalU64(
      options[5].substr(
          std::string("source_relation_descriptor_generation:").size()));
  descriptor.source_column_uuid.canonical =
      options[6].substr(std::string("source_column_uuid:").size());
  descriptor.source_column_descriptor_uuid.canonical =
      options[7].substr(
          std::string("source_column_descriptor_uuid:").size());
  const auto literal = ParseCanonicalI32(
      options[10].substr(
          std::string("expression_literal_value:").size()));
  descriptor.aggregate_function_uuid.canonical =
      options[12].substr(
          std::string("aggregate_function_uuid:").size());
  descriptor.result_alias =
      options[13].substr(std::string("aggregate_result_alias:").size());
  descriptor.result_descriptor =
      EngineGlobalAggregateAvgIntegerResultDescriptor();

  const std::string schema_uuid =
      PayloadFieldValue(record.payload, "schema=");
  const std::string target_uuid =
      PayloadFieldValue(record.payload, "target=");
  const std::set<std::string> persisted_identities = {
      descriptor.view_uuid.canonical,
      descriptor.view_descriptor_uuid.canonical,
      descriptor.source_relation_uuid.canonical,
      descriptor.source_relation_descriptor_uuid.canonical,
      descriptor.source_column_uuid.canonical,
      descriptor.source_column_descriptor_uuid.canonical,
      descriptor.aggregate_function_uuid.canonical};

  if (!view_generation || *view_generation == 0 || !source_generation ||
      *source_generation == 0 || !literal ||
      !SafeViewName(record.default_name) || target_uuid != record.object_uuid ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::schema,
                          schema_uuid) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          record.object_uuid) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.view_descriptor_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.source_relation_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.source_relation_descriptor_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.source_column_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.source_column_descriptor_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.aggregate_function_uuid.canonical) ||
      descriptor.aggregate_function_uuid.canonical !=
          EngineGlobalAggregateAvgFunctionUuid() ||
      !SafeAlias(descriptor.result_alias) || persisted_identities.size() != 7) {
    return std::nullopt;
  }
  descriptor.view_descriptor_generation = *view_generation;
  descriptor.source_relation_descriptor_generation = *source_generation;
  descriptor.expression_literal_int32 = *literal;
  descriptor.present = true;
  descriptor.diagnostic = OkDiagnostic();
  return descriptor;
}

const MgaRelationColumnStorageDescriptor* FindExactColumn(
    const MgaRelationStorageDescriptor& relation,
    std::string_view column_uuid,
    bool* duplicate = nullptr) {
  const MgaRelationColumnStorageDescriptor* found = nullptr;
  if (duplicate != nullptr) *duplicate = false;
  for (const auto& column : relation.columns) {
    if (column.column_uuid.canonical != column_uuid) continue;
    if (found != nullptr) {
      if (duplicate != nullptr) *duplicate = true;
      return nullptr;
    }
    found = &column;
  }
  return found;
}

struct VisibleApiBehaviorLookup {
  bool ok = false;
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::optional<ApiBehaviorRecord> record;
};

VisibleApiBehaviorLookup LookupVisibleApiBehaviorRecord(
    const EngineRequestContext& context,
    std::string_view object_uuid) {
  VisibleApiBehaviorLookup lookup;
  const auto loaded = LoadApiBehaviorState(context);
  if (!loaded.ok) {
    lookup.diagnostic = loaded.diagnostic;
    return lookup;
  }
  for (const auto& record : loaded.state.records) {
    if (record.object_uuid == object_uuid) {
      lookup.record = record;
      break;
    }
  }
  lookup.ok = true;
  return lookup;
}

bool CreateOrAlterRequested(const EngineApiRequest& request) {
  const auto values = OptionValues(request, "create_or_alter:");
  return request.operation_id == "ddl.create_or_alter_view" ||
         (values.size() == 1 && values.front() == "true");
}

bool RequestOptionsHaveExactShape(const EngineApiRequest& request,
                                  bool create_or_alter) {
  const std::set<std::string> admitted_prefixes = {
      "view_query_shape:",
      "source_relation_descriptor_uuid:",
      "source_relation_descriptor_generation:",
      "aggregate_function_uuid:",
      "aggregate_result_alias:",
      "create_or_alter:"};
  for (const auto& option : request.option_envelopes) {
    bool admitted = false;
    for (const auto& prefix : admitted_prefixes) {
      if (option.rfind(prefix, 0) == 0) {
        admitted = true;
        break;
      }
    }
    if (!admitted) return false;
  }
  const auto create_values = OptionValues(request, "create_or_alter:");
  return request.option_envelopes.size() ==
             static_cast<std::size_t>(create_or_alter ? 6 : 5) &&
         OptionValues(request, "view_query_shape:").size() == 1 &&
         OptionValues(request, "source_relation_descriptor_uuid:").size() ==
             1 &&
         OptionValues(request,
                      "source_relation_descriptor_generation:").size() == 1 &&
         OptionValues(request, "aggregate_function_uuid:").size() == 1 &&
         OptionValues(request, "aggregate_result_alias:").size() == 1 &&
         ((!create_or_alter && create_values.empty()) ||
          (create_or_alter && create_values.size() == 1 &&
           create_values.front() == "true"));
}

}  // namespace

bool IsEngineGlobalAggregateViewCreateRequest(
    const EngineApiRequest& request) {
  const auto markers = OptionValues(request, "view_query_shape:");
  return markers.size() == 1 &&
         markers.front() == kEngineGlobalAggregateViewMarkerV1;
}

EngineGlobalAggregateViewCreatePreparation
PrepareEngineGlobalAggregateViewCreate(const EngineApiRequest& request) {
  EngineGlobalAggregateViewCreatePreparation result;
  result.diagnostic = OkDiagnostic();
  if (!IsEngineGlobalAggregateViewCreateRequest(request)) {
    result.diagnostic = ViewDiagnostic("global_aggregate_view_marker_required");
    return result;
  }
  const auto exact_transaction =
      ValidateExactActiveTransaction(request.context, true);
  if (exact_transaction.error) {
    result.diagnostic = exact_transaction;
    return result;
  }
  const bool create_or_alter = CreateOrAlterRequested(request);
  if (!RequestOptionsHaveExactShape(request, create_or_alter) ||
      HasForbiddenRequestData(request) ||
      (request.target_object.object_kind != "view" &&
       !request.target_object.object_kind.empty()) ||
      request.target_schema.uuid.canonical.empty() ||
      (request.target_schema.object_kind != "schema" &&
       !request.target_schema.object_kind.empty()) ||
      request.related_objects.size() != 1 ||
      request.related_objects.front().object_kind != "table" ||
      request.related_objects.front().uuid.canonical.empty() ||
      request.columns.size() != 1 || request.columns.front().ordinal != 0 ||
      request.assignments.size() != 1 ||
      request.assignments.front().first != "int32_literal" ||
      request.descriptors.size() != 2 ||
      request.projection.canonical_projection_envelopes.size() != 1 ||
      request.projection.canonical_projection_envelopes.front() !=
          kEngineGlobalAggregateViewInt32MultiplyV1) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_request_shape_invalid");
    return result;
  }
  if (!CanonicalTypedUuid(scratchbird::core::platform::UuidKind::schema,
                          request.target_schema.uuid.canonical) ||
      !FindVisibleSchemaTreeRecord(
          request.context,
          request.target_schema.uuid.canonical,
          request.context.local_transaction_id)) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_schema_not_visible");
    return result;
  }

  const std::string view_name = ApiBehaviorPrimaryName(request, {});
  if (!SafeViewName(view_name)) {
    result.diagnostic = ViewDiagnostic("global_aggregate_view_name_invalid");
    return result;
  }
  const std::string aggregate_uuid =
      SingleOptionValue(request, "aggregate_function_uuid:");
  const std::string alias =
      SingleOptionValue(request, "aggregate_result_alias:");
  if (aggregate_uuid != EngineGlobalAggregateAvgFunctionUuid() ||
      !SafeAlias(alias) ||
      !DescriptorExactlyMatches(
          request.descriptors[0],
          EngineGlobalAggregateExpressionInt64ResultDescriptor()) ||
      !DescriptorExactlyMatches(
          request.descriptors[1],
          EngineGlobalAggregateAvgIntegerResultDescriptor())) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_result_contract_invalid");
    return result;
  }

  const std::string source_uuid =
      request.related_objects.front().uuid.canonical;
  const auto source =
      LoadMgaRelationStorageDescriptor(request.context, source_uuid);
  if (!source.ok) {
    result.diagnostic = source.diagnostic;
    return result;
  }
  const auto& relation = source.descriptor;
  const auto expected_source_generation = ParseCanonicalU64(
      SingleOptionValue(request,
                        "source_relation_descriptor_generation:"));
  if (relation.relation_uuid.canonical != source_uuid ||
      relation.relation_kind != "table" ||
      relation.descriptor_uuid.canonical !=
          SingleOptionValue(request, "source_relation_descriptor_uuid:") ||
      !expected_source_generation || *expected_source_generation == 0 ||
      relation.descriptor_generation != *expected_source_generation) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_source_descriptor_stale");
    return result;
  }
  bool duplicate_column = false;
  const auto* source_column = FindExactColumn(
      relation,
      request.columns.front().requested_column_uuid.canonical,
      &duplicate_column);
  if (duplicate_column || source_column == nullptr ||
      dt::CanonicalTypeIdFromStableName(
          source_column->value_descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int32 ||
      source_column->value_descriptor.descriptor_uuid.canonical.empty() ||
      !DescriptorExactlyMatches(request.columns.front().descriptor,
                                source_column->value_descriptor)) {
    result.diagnostic = ViewDiagnostic(
        duplicate_column
            ? "global_aggregate_view_source_column_ambiguous"
            : "global_aggregate_view_source_column_descriptor_stale");
    return result;
  }

  EngineGlobalAggregateProjection projection;
  projection.operation = EngineGlobalAggregateOperation::avg_field;
  projection.aggregate_function_uuid.canonical = aggregate_uuid;
  projection.source_field.column_uuid = source_column->column_uuid;
  projection.source_field.value_descriptor =
      source_column->value_descriptor;
  projection.input_expression.kind =
      EngineGlobalAggregateInputExpressionKind::
          int32_literal_times_int32_field_to_int64;
  projection.input_expression.int32_literal =
      request.assignments.front().second;
  projection.input_expression.result_descriptor = request.descriptors[0];
  projection.output_alias = alias;
  projection.result_descriptor = request.descriptors[1];
  EngineGlobalAggregateProjectionEnvelope aggregate_envelope;
  aggregate_envelope.relation_uuid = relation.relation_uuid;
  aggregate_envelope.relation_descriptor_uuid = relation.descriptor_uuid;
  aggregate_envelope.relation_descriptor_generation =
      relation.descriptor_generation;
  aggregate_envelope.outputs.push_back(projection);
  const auto aggregate_validation =
      ValidateGlobalAggregateProjectionEnvelope(aggregate_envelope);
  if (aggregate_validation.error) {
    result.diagnostic = aggregate_validation;
    return result;
  }
  const auto literal = ParseCanonicalI32(
      projection.input_expression.int32_literal.encoded_value);
  if (!literal) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_literal_int32_invalid");
    return result;
  }

  std::optional<ApiBehaviorRecord> existing;
  std::string view_uuid = request.target_object.uuid.canonical;
  if (!view_uuid.empty()) {
    const auto lookup =
        LookupVisibleApiBehaviorRecord(request.context, view_uuid);
    if (!lookup.ok) {
      result.diagnostic = lookup.diagnostic;
      return result;
    }
    existing = lookup.record;
    if (!existing) {
      result.diagnostic = ViewDiagnostic(
          "global_aggregate_view_uuid_must_be_engine_resolved");
      return result;
    }
  } else {
    EngineApiRequest resolve = request;
    resolve.option_envelopes.clear();
    const auto by_name = ResolveNameRegistryPrivate(resolve, "view");
    if (by_name.ok) {
      if (by_name.matches.size() != 1) {
        result.diagnostic = ViewDiagnostic(
            "global_aggregate_view_name_ambiguous");
        return result;
      }
      view_uuid = by_name.matches.front().object_uuid;
      const auto lookup =
          LookupVisibleApiBehaviorRecord(request.context, view_uuid);
      if (!lookup.ok) {
        result.diagnostic = lookup.diagnostic;
        return result;
      }
      existing = lookup.record;
    } else if (by_name.diagnostic.code != "CATALOG.NAME.NOT_FOUND") {
      result.diagnostic = by_name.diagnostic;
      return result;
    }
  }

  EngineGlobalAggregateViewDescriptor previous;
  if (existing) {
    const auto parsed = ParsePersistedDescriptor(*existing);
    if (!parsed || existing->object_kind != "view" ||
        existing->operation_id != "ddl.create_view" ||
        existing->state != "created" || existing->deleted ||
        existing->default_name != view_name ||
        PayloadFieldValue(existing->payload, "schema=") !=
            request.target_schema.uuid.canonical) {
      result.diagnostic = ViewDiagnostic(
          "global_aggregate_view_existing_descriptor_invalid");
      return result;
    }
    if (!create_or_alter) {
      result.diagnostic = ViewDiagnostic(
          "global_aggregate_view_already_exists");
      return result;
    }
    previous = *parsed;
    result.altered_existing = true;
  } else {
    if (!view_uuid.empty()) {
      result.diagnostic = ViewDiagnostic(
          "global_aggregate_view_existing_record_not_visible");
      return result;
    }
    view_uuid = GenerateCrudEngineUuid("object");
  }
  if (!CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          view_uuid) ||
      view_uuid == source_uuid ||
      view_uuid == relation.descriptor_uuid.canonical ||
      view_uuid == source_column->column_uuid.canonical ||
      view_uuid == source_column->value_descriptor.descriptor_uuid.canonical ||
      view_uuid == aggregate_uuid) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_uuid_allocation_failed");
    return result;
  }
  if (result.altered_existing &&
      previous.view_descriptor_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_descriptor_generation_overflow");
    return result;
  }

  EngineGlobalAggregateViewDescriptor descriptor;
  descriptor.present = true;
  descriptor.view_uuid.canonical = view_uuid;
  descriptor.view_descriptor_uuid.canonical =
      GenerateCrudEngineUuid("object");
  descriptor.view_descriptor_generation =
      result.altered_existing
          ? previous.view_descriptor_generation + 1
          : 1;
  descriptor.source_relation_uuid = relation.relation_uuid;
  descriptor.source_relation_descriptor_uuid = relation.descriptor_uuid;
  descriptor.source_relation_descriptor_generation =
      relation.descriptor_generation;
  descriptor.source_column_uuid = source_column->column_uuid;
  descriptor.source_column_descriptor_uuid =
      source_column->value_descriptor.descriptor_uuid;
  descriptor.expression_literal_int32 = *literal;
  descriptor.aggregate_function_uuid.canonical = aggregate_uuid;
  descriptor.result_alias = alias;
  descriptor.result_descriptor = request.descriptors[1];
  descriptor.diagnostic = OkDiagnostic();
  if (!CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.view_descriptor_uuid.canonical) ||
      descriptor.view_descriptor_uuid.canonical == view_uuid ||
      descriptor.view_descriptor_uuid.canonical == source_uuid ||
      descriptor.view_descriptor_uuid.canonical ==
          relation.descriptor_uuid.canonical ||
      descriptor.view_descriptor_uuid.canonical ==
          source_column->column_uuid.canonical ||
      descriptor.view_descriptor_uuid.canonical ==
          source_column->value_descriptor.descriptor_uuid.canonical ||
      descriptor.view_descriptor_uuid.canonical == aggregate_uuid ||
      (result.altered_existing &&
       descriptor.view_descriptor_uuid.canonical ==
           previous.view_descriptor_uuid.canonical)) {
    result.diagnostic = ViewDiagnostic(
        "global_aggregate_view_descriptor_uuid_allocation_failed");
    return result;
  }

  result.canonical_persisted_options = {
      std::string("view_query_shape:") +
          kEngineGlobalAggregateViewMarkerV1,
      "view_descriptor_uuid:" +
          descriptor.view_descriptor_uuid.canonical,
      "view_descriptor_generation:" +
          std::to_string(descriptor.view_descriptor_generation),
      "source_relation_uuid:" +
          descriptor.source_relation_uuid.canonical,
      "source_relation_descriptor_uuid:" +
          descriptor.source_relation_descriptor_uuid.canonical,
      "source_relation_descriptor_generation:" +
          std::to_string(
              descriptor.source_relation_descriptor_generation),
      "source_column_uuid:" + descriptor.source_column_uuid.canonical,
      "source_column_descriptor_uuid:" +
          descriptor.source_column_descriptor_uuid.canonical,
      std::string("expression_kind:") +
          kEngineGlobalAggregateViewInt32MultiplyV1,
      "expression_literal_type:int32",
      "expression_literal_value:" +
          std::to_string(descriptor.expression_literal_int32),
      "expression_result_type:int64",
      "aggregate_function_uuid:" +
          descriptor.aggregate_function_uuid.canonical,
      "aggregate_result_alias:" + descriptor.result_alias,
      "aggregate_result_type:int64_nullable"};
  result.descriptor = std::move(descriptor);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineGlobalAggregateViewDescriptor DescribeEngineGlobalAggregateView(
    const EngineRequestContext& context,
    const std::string& view_uuid) {
  EngineGlobalAggregateViewDescriptor descriptor;
  descriptor.diagnostic = OkDiagnostic();
  if (view_uuid.empty()) {
    descriptor.diagnostic = ViewDiagnostic(
        "global_aggregate_view_uuid_required");
    return descriptor;
  }
  const auto exact_transaction =
      ValidateExactActiveTransaction(context, false);
  if (exact_transaction.error) {
    descriptor.diagnostic = exact_transaction;
    return descriptor;
  }
  const auto lookup = LookupVisibleApiBehaviorRecord(context, view_uuid);
  if (!lookup.ok) {
    descriptor.diagnostic = lookup.diagnostic;
    return descriptor;
  }
  if (!lookup.record) {
    // No bounded descriptor is an ordinary/non-visible view classification.
    // Public name visibility remains owned by the catalog/name registry; this
    // descriptor helper must not manufacture visibility on its own.
    return descriptor;
  }
  const auto& record = *lookup.record;
  const auto options = PayloadOptions(record.payload);
  if (!PayloadMentionsBoundedViewFamily(options)) {
    if (record.payload.find("engine.global_aggregate_view") !=
        std::string::npos) {
      descriptor.diagnostic = ViewDiagnostic(
          "global_aggregate_view_descriptor_invalid");
    }
    return descriptor;
  }
  const auto parsed = ParsePersistedDescriptor(record);
  if (!PayloadRequestsBoundedView(options) || !parsed ||
      record.object_kind != "view" ||
      record.operation_id != "ddl.create_view" ||
      record.state != "created" || record.deleted) {
    descriptor.diagnostic = ViewDiagnostic(
        "global_aggregate_view_descriptor_invalid");
    return descriptor;
  }
  return *parsed;
}

EngineDescriptor EngineGlobalAggregateViewSemanticDescriptor(
    const EngineGlobalAggregateViewDescriptor& descriptor) {
  EngineDescriptor semantic;
  if (!descriptor.present || descriptor.view_uuid.canonical.empty() ||
      descriptor.view_descriptor_uuid.canonical.empty() ||
      descriptor.view_descriptor_generation == 0 ||
      !SafeAlias(descriptor.result_alias) ||
      !DescriptorExactlyMatches(
          descriptor.result_descriptor,
          EngineGlobalAggregateAvgIntegerResultDescriptor())) {
    return semantic;
  }
  semantic.descriptor_uuid = descriptor.view_descriptor_uuid;
  semantic.descriptor_kind = "global_aggregate_view";
  semantic.canonical_type_name = kEngineGlobalAggregateViewMarkerV1;
  semantic.encoded_descriptor =
      std::string("marker=") + kEngineGlobalAggregateViewMarkerV1 +
      ";view_uuid=" + descriptor.view_uuid.canonical +
      ";view_descriptor_generation=" +
      std::to_string(descriptor.view_descriptor_generation) +
      ";result_alias=" + descriptor.result_alias +
      ";result_type=int64;result_nullable=true";
  return semantic;
}

bool IsEngineGlobalAggregateViewSelectRequest(
    const EngineSelectRowsRequest& request) {
  if (request.source_object.object_kind != "view") return false;
  const auto& select =
      request.select_projection.canonical_projection_envelopes;
  const auto& base = request.projection.canonical_projection_envelopes;
  const auto mentions_marker = [](const std::vector<std::string>& values) {
    return std::any_of(values.begin(), values.end(), [](const auto& value) {
      return value.rfind("engine.global_aggregate_view", 0) == 0;
    });
  };
  return mentions_marker(select) || mentions_marker(base);
}

EngineApiDiagnostic ExpandEngineGlobalAggregateViewSelect(
    const EngineSelectRowsRequest& request,
    EngineSelectRowsRequest* expanded,
    EngineGlobalAggregateViewDescriptor* descriptor_out) {
  if (expanded == nullptr || descriptor_out == nullptr) {
    return ViewDiagnostic("global_aggregate_view_expansion_output_required");
  }
  const auto& select =
      request.select_projection.canonical_projection_envelopes;
  const auto& base = request.projection.canonical_projection_envelopes;
  const bool exact_select =
      select.size() == 1 &&
      select.front() == kEngineGlobalAggregateViewMarkerV1 && base.empty();
  const bool exact_base =
      base.size() == 1 &&
      base.front() == kEngineGlobalAggregateViewMarkerV1 && select.empty();
  if ((!exact_select && !exact_base) ||
      request.source_object.uuid.canonical.empty() ||
      request.descriptors.size() != 1 ||
      !request.global_aggregate_projection.outputs.empty() ||
      !request.select_predicate.predicate_kind.empty() ||
      !request.predicate.predicate_kind.empty() ||
      !request.select_ordering.canonical_ordering_envelopes.empty() ||
      !request.ordering.canonical_ordering_envelopes.empty() ||
      request.limit != 0 || request.offset != 0 ||
      !request.option_envelopes.empty() || !request.rows.empty() ||
      !request.assignments.empty()) {
    return ViewDiagnostic("global_aggregate_view_select_shape_invalid");
  }

  auto descriptor = DescribeEngineGlobalAggregateView(
      request.context, request.source_object.uuid.canonical);
  if (descriptor.diagnostic.error) return descriptor.diagnostic;
  if (!descriptor.present) {
    return ViewDiagnostic("global_aggregate_view_descriptor_required");
  }
  const auto semantic =
      EngineGlobalAggregateViewSemanticDescriptor(descriptor);
  if (!DescriptorExactlyMatches(request.descriptors.front(), semantic)) {
    return ViewDiagnostic("global_aggregate_view_descriptor_stale");
  }

  const auto source = LoadMgaRelationStorageDescriptor(
      request.context, descriptor.source_relation_uuid.canonical);
  if (!source.ok) return source.diagnostic;
  const auto& relation = source.descriptor;
  if (relation.relation_uuid.canonical !=
          descriptor.source_relation_uuid.canonical ||
      relation.descriptor_uuid.canonical !=
          descriptor.source_relation_descriptor_uuid.canonical ||
      relation.descriptor_generation !=
          descriptor.source_relation_descriptor_generation) {
    return ViewDiagnostic(
        "global_aggregate_view_source_descriptor_stale");
  }
  bool duplicate_column = false;
  const auto* source_column = FindExactColumn(
      relation,
      descriptor.source_column_uuid.canonical,
      &duplicate_column);
  if (duplicate_column || source_column == nullptr ||
      source_column->value_descriptor.descriptor_uuid.canonical !=
          descriptor.source_column_descriptor_uuid.canonical ||
      dt::CanonicalTypeIdFromStableName(
          source_column->value_descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int32) {
    return ViewDiagnostic(
        duplicate_column
            ? "global_aggregate_view_source_column_ambiguous"
            : "global_aggregate_view_source_column_descriptor_stale");
  }

  EngineSelectRowsRequest next;
  next.context = request.context;
  next.context.request_id = request.context.request_id;
  next.source_object.uuid = relation.relation_uuid;
  next.source_object.object_kind = "table";
  next.global_aggregate_projection.relation_uuid = relation.relation_uuid;
  next.global_aggregate_projection.relation_descriptor_uuid =
      relation.descriptor_uuid;
  next.global_aggregate_projection.relation_descriptor_generation =
      relation.descriptor_generation;
  EngineGlobalAggregateProjection projection;
  projection.operation = EngineGlobalAggregateOperation::avg_field;
  projection.aggregate_function_uuid = descriptor.aggregate_function_uuid;
  projection.source_field.column_uuid = source_column->column_uuid;
  projection.source_field.value_descriptor =
      source_column->value_descriptor;
  projection.input_expression.kind =
      EngineGlobalAggregateInputExpressionKind::
          int32_literal_times_int32_field_to_int64;
  projection.input_expression.int32_literal.descriptor =
      EngineGlobalAggregateExpressionInt32LiteralDescriptor();
  projection.input_expression.int32_literal.encoded_value =
      std::to_string(descriptor.expression_literal_int32);
  projection.input_expression.int32_literal.state = EngineValueState::value;
  projection.input_expression.result_descriptor =
      EngineGlobalAggregateExpressionInt64ResultDescriptor();
  projection.output_alias = descriptor.result_alias;
  projection.result_descriptor = descriptor.result_descriptor;
  next.global_aggregate_projection.outputs.push_back(std::move(projection));

  const auto validated = ValidateGlobalAggregateProjectionEnvelope(
      next.global_aggregate_projection);
  if (validated.error) return validated;
  *expanded = std::move(next);
  *descriptor_out = std::move(descriptor);
  return OkDiagnostic();
}

}  // namespace scratchbird::engine::internal_api
