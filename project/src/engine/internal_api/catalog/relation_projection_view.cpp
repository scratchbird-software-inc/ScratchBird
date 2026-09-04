// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/relation_projection_view.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "datatype_operations.hpp"
#include "dml/delete_api.hpp"
#include "dml/select_api.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction_inventory.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <charconv>
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

namespace dt = scratchbird::core::datatypes;

constexpr std::string_view kOperation = "catalog.relation_projection_view";
constexpr std::size_t kPersistedOptionCountV1 = 30;
constexpr std::size_t kPersistedOptionCountV2 = 19;

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

bool DescriptorTypeShapeMatches(
    const EngineRelationProjectionTypeDescriptor& left,
    const EngineDescriptor& right) {
  return left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool TypeDescriptorExactlyMatches(
    const EngineRelationProjectionTypeDescriptor& left,
    const EngineRelationProjectionTypeDescriptor& right) {
  return left.type_descriptor_uuid.canonical ==
             right.type_descriptor_uuid.canonical &&
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

bool SafeUnquotedIdentifier(std::string_view identifier) {
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

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool CanonicalTypedUuid(scratchbird::core::platform::UuidKind kind,
                        std::string_view value) {
  const auto parsed = scratchbird::core::uuid::ParseTypedUuid(
      kind, std::string(value));
  return parsed.ok() &&
         scratchbird::core::uuid::UuidToString(parsed.value.value) == value;
}

bool LocalizedNameIsUnquoted(const EngineLocalizedName& name) {
  return !name.was_quoted &&
         (name.quote_style.empty() || name.quote_style == "none") &&
         !name.requires_exact_match;
}

std::string ExactOutputName(const EngineColumnDefinition& column) {
  if (column.names.size() != 1 ||
      !LocalizedNameIsUnquoted(column.names.front()) ||
      !SafeUnquotedIdentifier(column.names.front().name)) {
    return {};
  }
  return column.names.front().name;
}

std::string ExactViewName(const EngineApiRequest& request) {
  if (request.localized_names.size() != 1 ||
      !LocalizedNameIsUnquoted(request.localized_names.front()) ||
      !SafeUnquotedIdentifier(request.localized_names.front().name)) {
    return {};
  }
  return request.localized_names.front().name;
}

bool HasForbiddenRequestData(const EngineApiRequest& request) {
  if (!request.rows.empty() || !request.constraints.empty() ||
      !request.indexes.empty() || request.native_row_packet.present ||
      !request.native_row_packet.packet_bytes.empty() ||
      !request.shared_row_field_order.empty() ||
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
      !request.diagnostic_options.empty() ||
      !request.sql_object_reference.path_components.empty() ||
      !request.sql_object_reference.object_name.raw_text.empty() ||
      !request.bound_object_identity.object_uuid.canonical.empty()) {
    return true;
  }
  for (const auto& option : request.option_envelopes) {
    const std::string lowered = LowerAscii(option);
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

std::string HexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[(ch >> 4U) & 0x0fU]);
    encoded.push_back(kHex[ch & 0x0fU]);
  }
  return encoded;
}

std::optional<std::string> HexDecode(std::string_view value) {
  if ((value.size() % 2) != 0) return std::nullopt;
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  std::string decoded;
  decoded.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int high = nibble(value[i]);
    const int low = nibble(value[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  return decoded;
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

bool PayloadRequestsRelationProjectionView(
    const std::vector<std::string>& options) {
  const auto count_marker = [&](const char* marker) {
    return std::count(options.begin(),
                      options.end(),
                      std::string("view_query_shape:") + marker);
  };
  return count_marker(kEngineRelationProjectionViewMarkerV1) +
             count_marker(kEngineRelationProjectionViewMarkerV2) ==
         1;
}

bool PayloadMentionsRelationProjectionViewFamily(
    const std::vector<std::string>& options) {
  return std::any_of(options.begin(), options.end(), [](const auto& option) {
    return option.rfind(
               "view_query_shape:engine.relation_projection_view", 0) == 0;
  });
}

std::optional<std::string> OptionSuffix(const std::string& option,
                                        std::string_view prefix) {
  if (option.rfind(prefix, 0) != 0) return std::nullopt;
  return option.substr(prefix.size());
}

std::optional<EngineRelationProjectionViewOutput> ParseCommonOutput(
    const std::vector<std::string>& options,
    std::size_t offset,
    std::uint32_t ordinal) {
  const std::string prefix = "output_" + std::to_string(ordinal) + "_";
  const auto column_uuid =
      OptionSuffix(options[offset], prefix + "column_uuid:");
  const auto expression_uuid =
      OptionSuffix(options[offset + 1], prefix + "expression_uuid:");
  const auto name_hex =
      OptionSuffix(options[offset + 2], prefix + "name_hex:");
  const auto type_uuid = OptionSuffix(
      options[offset + 3], prefix + "type_descriptor_uuid:");
  const auto type_kind_hex = OptionSuffix(
      options[offset + 4], prefix + "type_descriptor_kind_hex:");
  const auto type_name_hex = OptionSuffix(
      options[offset + 5], prefix + "type_canonical_name_hex:");
  const auto type_encoded_hex = OptionSuffix(
      options[offset + 6], prefix + "type_encoded_descriptor_hex:");
  const auto nullable =
      OptionSuffix(options[offset + 7], prefix + "nullable:");
  const auto expression_kind =
      OptionSuffix(options[offset + 8], prefix + "expression_kind:");
  if (!column_uuid || !expression_uuid || !name_hex || !type_uuid ||
      !type_kind_hex || !type_name_hex || !type_encoded_hex || !nullable ||
      !expression_kind) {
    return std::nullopt;
  }
  const auto name = HexDecode(*name_hex);
  const auto type_kind = HexDecode(*type_kind_hex);
  const auto type_name = HexDecode(*type_name_hex);
  const auto type_encoded = HexDecode(*type_encoded_hex);
  if (!name || !type_kind || !type_name || !type_encoded ||
      (*nullable != "true" && *nullable != "false")) {
    return std::nullopt;
  }

  EngineRelationProjectionViewOutput output;
  output.ordinal = ordinal;
  output.output_column_uuid.canonical = *column_uuid;
  output.expression_uuid.canonical = *expression_uuid;
  output.output_name = *name;
  output.output_type.type_descriptor_uuid.canonical = *type_uuid;
  output.output_type.descriptor_kind = *type_kind;
  output.output_type.canonical_type_name = *type_name;
  output.output_type.encoded_descriptor = *type_encoded;
  output.nullable = *nullable == "true";
  if (*expression_kind == kEngineRelationProjectionSourceColumnV1) {
    output.expression_kind =
        EngineRelationProjectionExpressionKind::source_column;
  } else if (*expression_kind ==
             kEngineRelationProjectionTypedInt32LiteralV1) {
    output.expression_kind =
        EngineRelationProjectionExpressionKind::typed_int32_literal;
  } else {
    return std::nullopt;
  }
  return output;
}

std::optional<EngineRelationProjectionViewDescriptor>
ParsePersistedDescriptor(const ApiBehaviorRecord& record) {
  const auto options = PayloadOptions(record.payload);
  if (!PayloadRequestsRelationProjectionView(options)) return std::nullopt;
  const bool v1 = options[0] ==
                  std::string("view_query_shape:") +
                      kEngineRelationProjectionViewMarkerV1;
  const bool v2 = options[0] ==
                  std::string("view_query_shape:") +
                      kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_option_count =
      v1 ? kPersistedOptionCountV1 : kPersistedOptionCountV2;
  const std::string expected_output_count = v1 ? "output_count:2"
                                                : "output_count:1";
  if ((!v1 && !v2) || options.size() != expected_option_count ||
      options[1].rfind("view_descriptor_uuid:", 0) != 0 ||
      options[2].rfind("view_descriptor_generation:", 0) != 0 ||
      options[3].rfind("source_relation_uuid:", 0) != 0 ||
      options[4].rfind("source_relation_descriptor_uuid:", 0) != 0 ||
      options[5].rfind("source_relation_descriptor_generation:", 0) != 0 ||
      options[6].rfind("source_resource_epoch:", 0) != 0 ||
      options[7] != expected_output_count) {
    return std::nullopt;
  }

  EngineRelationProjectionViewDescriptor descriptor;
  descriptor.marker = v1 ? kEngineRelationProjectionViewMarkerV1
                         : kEngineRelationProjectionViewMarkerV2;
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
  const auto source_resource_epoch = ParseCanonicalU64(
      options[6].substr(std::string("source_resource_epoch:").size()));

  auto source_output = ParseCommonOutput(options, 8, 0);
  if (!source_output ||
      source_output->expression_kind !=
          EngineRelationProjectionExpressionKind::source_column) {
    return std::nullopt;
  }
  const auto source_column_uuid =
      OptionSuffix(options[17], "output_0_source_column_uuid:");
  const auto source_type_uuid = OptionSuffix(
      options[18], "output_0_source_column_type_descriptor_uuid:");
  if (!source_column_uuid || !source_type_uuid) return std::nullopt;
  source_output->source_column_uuid.canonical = *source_column_uuid;
  source_output->source_column_type_descriptor_uuid.canonical =
      *source_type_uuid;

  std::optional<EngineRelationProjectionViewOutput> literal_output;
  if (v1) {
    literal_output = ParseCommonOutput(options, 19, 1);
    if (!literal_output ||
        literal_output->expression_kind !=
            EngineRelationProjectionExpressionKind::typed_int32_literal ||
        options[28] != "output_1_literal_type:int32") {
      return std::nullopt;
    }
    const auto literal = OptionSuffix(
        options[29], "output_1_literal_value:");
    const auto parsed_literal = literal ? ParseCanonicalI32(*literal)
                                        : std::nullopt;
    if (!parsed_literal) return std::nullopt;
    literal_output->literal_int32 = *parsed_literal;
  }

  if (!view_generation || *view_generation == 0 || !source_generation ||
      *source_generation == 0 || !source_resource_epoch ||
      *source_resource_epoch == 0) {
    return std::nullopt;
  }
  descriptor.view_descriptor_generation = *view_generation;
  descriptor.source_relation_descriptor_generation = *source_generation;
  descriptor.source_resource_epoch = *source_resource_epoch;
  descriptor.outputs.push_back(std::move(*source_output));
  if (literal_output) {
    descriptor.outputs.push_back(std::move(*literal_output));
  }

  EngineRelationProjectionEnvelope envelope;
  envelope.marker = descriptor.marker;
  envelope.relation_uuid = descriptor.source_relation_uuid;
  envelope.relation_descriptor_uuid =
      descriptor.source_relation_descriptor_uuid;
  envelope.relation_descriptor_generation =
      descriptor.source_relation_descriptor_generation;
  envelope.source_resource_epoch = descriptor.source_resource_epoch;
  envelope.outputs = descriptor.outputs;
  const auto validated = ValidateEngineRelationProjectionEnvelope(envelope);

  const std::string schema_uuid =
      PayloadFieldValue(record.payload, "schema=");
  const std::string target_uuid =
      PayloadFieldValue(record.payload, "target=");
  if (validated.error || !SafeUnquotedIdentifier(record.default_name) ||
      target_uuid != record.object_uuid ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::schema,
                          schema_uuid) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          record.object_uuid) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          descriptor.view_descriptor_uuid.canonical)) {
    return std::nullopt;
  }

  std::set<std::string> identities = {
      record.object_uuid,
      descriptor.view_descriptor_uuid.canonical,
      descriptor.source_relation_uuid.canonical,
      descriptor.source_relation_descriptor_uuid.canonical};
  for (const auto& output : descriptor.outputs) {
    identities.insert(output.output_column_uuid.canonical);
    identities.insert(output.output_type.type_descriptor_uuid.canonical);
    identities.insert(output.expression_uuid.canonical);
  }
  identities.insert(descriptor.outputs[0].source_column_uuid.canonical);
  const bool source_type_identity_conflicts =
      descriptor.outputs[0].source_column_type_descriptor_uuid.canonical !=
          descriptor.outputs[0].source_column_uuid.canonical &&
      identities.contains(
          descriptor.outputs[0].source_column_type_descriptor_uuid.canonical);
  if (source_type_identity_conflicts ||
      identities.size() != (v1 ? 11u : 8u)) {
    return std::nullopt;
  }

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

bool RequestOptionsHaveExactShape(const EngineApiRequest& request) {
  if (request.option_envelopes.size() != 4 ||
      OptionValues(request, "view_query_shape:").size() != 1 ||
      OptionValues(request, "source_relation_descriptor_uuid:").size() != 1 ||
      OptionValues(request,
                   "source_relation_descriptor_generation:").size() != 1 ||
      OptionValues(request, "source_resource_epoch:").size() != 1) {
    return false;
  }
  for (const auto& option : request.option_envelopes) {
    if (option.rfind("view_query_shape:", 0) != 0 &&
        option.rfind("source_relation_descriptor_uuid:", 0) != 0 &&
        option.rfind("source_relation_descriptor_generation:", 0) != 0 &&
        option.rfind("source_resource_epoch:", 0) != 0) {
      return false;
    }
  }
  return true;
}

struct VisibleIdentityInventory {
  bool ok = false;
  EngineApiDiagnostic diagnostic = OkDiagnostic();
  std::set<std::string> exact_identities;
  std::vector<std::string> encoded_identity_payloads;
};

VisibleIdentityInventory LoadVisibleIdentityInventory(
    const EngineRequestContext& context) {
  VisibleIdentityInventory inventory;
  const auto remember = [&](const std::string& identity) {
    if (!identity.empty()) inventory.exact_identities.insert(identity);
  };
  const auto behaviors = LoadApiBehaviorState(context);
  if (!behaviors.ok) {
    inventory.diagnostic = behaviors.diagnostic;
    return inventory;
  }
  for (const auto& record : behaviors.state.records) {
    remember(record.object_uuid);
    inventory.encoded_identity_payloads.push_back(record.payload);
  }
  const auto names =
      LoadNameRegistryState(context, context.local_transaction_id);
  if (!names.ok) {
    inventory.diagnostic = names.diagnostic;
    return inventory;
  }
  for (const auto& entry : names.state.entries) {
    remember(entry.name_entry_uuid);
    remember(entry.object_uuid);
    remember(entry.scope_uuid);
    remember(entry.parent_object_uuid);
    remember(entry.parent_schema_uuid);
    remember(entry.reference_id);
    remember(entry.dialect_profile_uuid);
    remember(entry.identifier_profile_uuid);
    remember(entry.case_fold_profile_uuid);
    remember(entry.quoted_identifier_profile_uuid);
  }

  // CREATE VIEW is a bounded metadata operation, so a fail-closed full
  // metadata inventory is acceptable here.  This closes collisions not
  // represented by the public name registry, including relation/column/type
  // descriptors and physical index identities.
  const auto relations = LoadMgaRelationStoreState(context);
  if (!relations.ok) {
    inventory.diagnostic = relations.diagnostic;
    return inventory;
  }
  const CrudState visible_crud =
      BuildCrudCompatibilityStateFromMga(relations.state);
  std::set<std::string> visited_relations;
  for (const auto& table_record : visible_crud.tables) {
    const auto visible = FindVisibleCrudTable(
        visible_crud, table_record.table_uuid, context.local_transaction_id);
    if (!visible ||
        !visited_relations.insert(visible->table_uuid).second) {
      continue;
    }
    remember(visible->table_uuid);
    const auto relation =
        LoadMgaRelationStorageDescriptor(context, visible->table_uuid);
    if (!relation.ok) {
      inventory.diagnostic = relation.diagnostic;
      return inventory;
    }
    const auto& descriptor = relation.descriptor;
    remember(descriptor.descriptor_uuid.canonical);
    remember(descriptor.database_uuid.canonical);
    remember(descriptor.schema_uuid.canonical);
    remember(descriptor.relation_uuid.canonical);
    remember(descriptor.primary_filespace_uuid.canonical);
    for (const auto& column : descriptor.columns) {
      remember(column.column_uuid.canonical);
      remember(column.value_descriptor.descriptor_uuid.canonical);
      remember(column.charset_uuid);
      remember(column.collation_uuid);
    }
    for (const auto& index : descriptor.indexes) {
      remember(index.index_uuid.canonical);
    }
  }
  for (const auto& index : visible_crud.indexes) {
    remember(index.index_uuid);
    remember(index.table_uuid);
  }
  for (const auto& row : visible_crud.row_versions) {
    remember(row.table_uuid);
    remember(row.row_uuid);
    remember(row.version_uuid);
    remember(row.previous_version_uuid);
    for (const auto& [field, value] : row.values) {
      (void)field;
      remember(value);
    }
  }
  for (const auto& entry : visible_crud.index_entries) {
    remember(entry.index_uuid);
    remember(entry.table_uuid);
    remember(entry.row_uuid);
    remember(entry.version_uuid);
  }
  for (const auto& large_value : visible_crud.large_values) {
    remember(large_value.overflow_uuid);
    remember(large_value.table_uuid);
    remember(large_value.row_uuid);
    remember(large_value.version_uuid);
    for (const auto& chunk : large_value.chunks) {
      remember(chunk.overflow_uuid);
    }
  }
  inventory.ok = true;
  return inventory;
}

bool IdentityAlreadyVisible(const VisibleIdentityInventory& inventory,
                            std::string_view candidate) {
  if (inventory.exact_identities.count(std::string(candidate)) != 0) {
    return true;
  }
  return std::any_of(
      inventory.encoded_identity_payloads.begin(),
      inventory.encoded_identity_payloads.end(),
      [candidate](const std::string& payload) {
        return payload.find(candidate) != std::string::npos;
      });
}

std::optional<std::string> AllocateDistinctObjectUuid(
    const VisibleIdentityInventory& visible_inventory,
    std::set<std::string>* identities) {
  if (!visible_inventory.ok || identities == nullptr) return std::nullopt;
  for (std::size_t attempt = 0; attempt < 16; ++attempt) {
    const std::string candidate = GenerateCrudEngineUuid("object");
    if (CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                           candidate) &&
        !IdentityAlreadyVisible(visible_inventory, candidate) &&
        identities->insert(candidate).second) {
      return candidate;
    }
  }
  return std::nullopt;
}

EngineRelationProjectionTypeDescriptor AllocateOutputType(
    const EngineDescriptor& source,
    const VisibleIdentityInventory& visible_inventory,
    std::set<std::string>* identities) {
  EngineRelationProjectionTypeDescriptor type;
  const auto uuid =
      AllocateDistinctObjectUuid(visible_inventory, identities);
  if (!uuid) return type;
  type.type_descriptor_uuid.canonical = *uuid;
  type.descriptor_kind = source.descriptor_kind;
  type.canonical_type_name = source.canonical_type_name;
  type.encoded_descriptor = source.encoded_descriptor;
  return type;
}

std::vector<std::string> PersistedOptions(
    const EngineRelationProjectionViewDescriptor& descriptor) {
  const auto common = [](const EngineRelationProjectionViewOutput& output) {
    const std::string prefix =
        "output_" + std::to_string(output.ordinal) + "_";
    const char* expression_kind =
        output.expression_kind ==
                EngineRelationProjectionExpressionKind::source_column
            ? kEngineRelationProjectionSourceColumnV1
            : kEngineRelationProjectionTypedInt32LiteralV1;
    return std::vector<std::string>{
        prefix + "column_uuid:" + output.output_column_uuid.canonical,
        prefix + "expression_uuid:" + output.expression_uuid.canonical,
        prefix + "name_hex:" + HexEncode(output.output_name),
        prefix + "type_descriptor_uuid:" +
            output.output_type.type_descriptor_uuid.canonical,
        prefix + "type_descriptor_kind_hex:" +
            HexEncode(output.output_type.descriptor_kind),
        prefix + "type_canonical_name_hex:" +
            HexEncode(output.output_type.canonical_type_name),
        prefix + "type_encoded_descriptor_hex:" +
            HexEncode(output.output_type.encoded_descriptor),
        prefix + "nullable:" + (output.nullable ? "true" : "false"),
        prefix + "expression_kind:" + expression_kind};
  };

  const bool v1 = descriptor.marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = descriptor.marker == kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if ((!v1 && !v2) || descriptor.outputs.size() != expected_output_count) {
    return {};
  }
  std::vector<std::string> options = {
      std::string("view_query_shape:") + descriptor.marker,
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
      "source_resource_epoch:" +
          std::to_string(descriptor.source_resource_epoch),
      "output_count:" + std::to_string(expected_output_count)};
  auto source = common(descriptor.outputs[0]);
  options.insert(options.end(), source.begin(), source.end());
  options.push_back(
      "output_0_source_column_uuid:" +
      descriptor.outputs[0].source_column_uuid.canonical);
  options.push_back(
      "output_0_source_column_type_descriptor_uuid:" +
      descriptor.outputs[0].source_column_type_descriptor_uuid.canonical);
  if (v2) return options;
  auto literal = common(descriptor.outputs[1]);
  options.insert(options.end(), literal.begin(), literal.end());
  options.push_back("output_1_literal_type:int32");
  options.push_back("output_1_literal_value:" +
                    std::to_string(descriptor.outputs[1].literal_int32));
  return options;
}

bool SemanticOutputExactlyMatches(
    const EngineRelationProjectionViewSemanticOutput& semantic,
    const EngineRelationProjectionViewOutput& persisted) {
  return semantic.ordinal == persisted.ordinal &&
         semantic.output_column_uuid.canonical ==
             persisted.output_column_uuid.canonical &&
         semantic.output_name == persisted.output_name &&
         TypeDescriptorExactlyMatches(semantic.output_type,
                                      persisted.output_type) &&
         semantic.nullable == persisted.nullable;
}

struct StoredFieldValueLookup {
  bool found = false;
  bool duplicate = false;
  std::string_view value;
};

StoredFieldValueLookup StoredFieldValueExact(
    const CrudRowVersionRecord& row,
    std::string_view name_key) {
  StoredFieldValueLookup lookup;
  for (const auto& [name, value] : row.values) {
    if (name != name_key) continue;
    if (lookup.found) {
      lookup.duplicate = true;
      continue;
    }
    lookup.found = true;
    lookup.value = value;
  }
  return lookup;
}

bool CanonicalInt32Value(const EngineDescriptor& descriptor,
                         std::string_view encoded,
                         std::string* canonical) {
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      descriptor.canonical_type_name);
  if (type_id != dt::CanonicalTypeId::int32) return false;
  dt::DatatypeCastRequest cast;
  cast.value.type_id = type_id;
  cast.value.encoded_value = std::string(encoded);
  cast.value.is_null = false;
  cast.target_type_id = type_id;
  cast.explicit_cast = false;
  const auto result = dt::CastDatatypeValue(cast);
  if (!result.ok() || result.value.is_null) return false;
  std::int32_t parsed = 0;
  const char* begin = result.value.encoded_value.data();
  const char* end = begin + result.value.encoded_value.size();
  const auto [parsed_end, error] = std::from_chars(begin, end, parsed);
  if (error != std::errc{} || parsed_end != end) return false;
  if (canonical != nullptr) *canonical = result.value.encoded_value;
  return true;
}

}  // namespace

EngineDescriptor EngineRelationProjectionInt32LiteralInputDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int32";
  descriptor.encoded_descriptor =
      "canonical=int32;precision=32;scale=0;nullable=false";
  return descriptor;
}

EngineDescriptor EngineRelationProjectionOutputTypeDescriptor(
    const EngineRelationProjectionTypeDescriptor& descriptor) {
  EngineDescriptor result;
  result.descriptor_uuid = descriptor.type_descriptor_uuid;
  result.descriptor_kind = descriptor.descriptor_kind;
  result.canonical_type_name = descriptor.canonical_type_name;
  result.encoded_descriptor = descriptor.encoded_descriptor;
  return result;
}

bool IsEngineRelationProjectionViewCreateRequest(
    const EngineApiRequest& request) {
  const auto markers = OptionValues(request, "view_query_shape:");
  return markers.size() == 1 &&
         (markers.front() == kEngineRelationProjectionViewMarkerV1 ||
          markers.front() == kEngineRelationProjectionViewMarkerV2);
}

EngineRelationProjectionViewCreatePreparation
PrepareEngineRelationProjectionViewCreate(const EngineApiRequest& request) {
  EngineRelationProjectionViewCreatePreparation result;
  result.diagnostic = OkDiagnostic();
  if (!IsEngineRelationProjectionViewCreateRequest(request)) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_marker_required");
    return result;
  }
  const std::string marker =
      SingleOptionValue(request, "view_query_shape:");
  const bool v1 = marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = marker == kEngineRelationProjectionViewMarkerV2;
  const auto exact_transaction =
      ValidateExactActiveTransaction(request.context, true);
  if (exact_transaction.error) {
    result.diagnostic = exact_transaction;
    return result;
  }
  if (!RequestOptionsHaveExactShape(request) ||
      HasForbiddenRequestData(request) ||
      (!request.operation_id.empty() &&
       request.operation_id != "ddl.create_view") ||
      !request.target_object.uuid.canonical.empty() ||
      (request.target_object.object_kind != "view" &&
       !request.target_object.object_kind.empty()) ||
      request.target_schema.uuid.canonical.empty() ||
      (request.target_schema.object_kind != "schema" &&
       !request.target_schema.object_kind.empty()) ||
      request.related_objects.size() != 1 ||
      request.related_objects.front().object_kind != "table" ||
      request.related_objects.front().uuid.canonical.empty() ||
      request.descriptors.size() != 0 ||
      (v1 &&
       (request.columns.size() != 2 || request.assignments.size() != 1 ||
        request.assignments.front().first != "projection_1_literal" ||
        request.projection.canonical_projection_envelopes !=
            std::vector<std::string>{
                kEngineRelationProjectionSourceColumnV1,
                kEngineRelationProjectionTypedInt32LiteralV1})) ||
      (v2 &&
       (request.columns.size() != 1 || !request.assignments.empty() ||
        request.projection.canonical_projection_envelopes !=
            std::vector<std::string>{
                kEngineRelationProjectionSourceColumnV1}))) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_request_shape_invalid");
    return result;
  }
  if (!CanonicalTypedUuid(scratchbird::core::platform::UuidKind::schema,
                          request.target_schema.uuid.canonical) ||
      !FindVisibleSchemaTreeRecord(
          request.context,
          request.target_schema.uuid.canonical,
          request.context.local_transaction_id)) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_schema_not_visible");
    return result;
  }

  const std::string view_name = ExactViewName(request);
  const std::string source_output_name = ExactOutputName(request.columns[0]);
  const std::string literal_output_name =
      v1 ? ExactOutputName(request.columns[1]) : std::string{};
  if (view_name.empty()) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_name_invalid_or_quoted");
    return result;
  }
  if (source_output_name.empty() ||
      (v1 &&
       (literal_output_name.empty() ||
        LowerAscii(source_output_name) == LowerAscii(literal_output_name)))) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_output_name_invalid");
    return result;
  }

  const auto& source_request = request.columns[0];
  if (source_request.ordinal != 0 ||
      source_request.requested_column_uuid.canonical.empty() ||
      !source_request.default_expression_envelope.empty()) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_output_contract_invalid");
    return result;
  }
  std::optional<std::int32_t> parsed_literal;
  if (v1) {
    const auto& literal_request = request.columns[1];
    const auto& literal_value = request.assignments.front().second;
    if (literal_request.ordinal != 1 ||
        !literal_request.requested_column_uuid.canonical.empty() ||
        literal_request.nullable ||
        !literal_request.default_expression_envelope.empty() ||
        !DescriptorExactlyMatches(
            literal_request.descriptor,
            EngineRelationProjectionInt32LiteralInputDescriptor()) ||
        !DescriptorExactlyMatches(literal_value.descriptor,
                                  literal_request.descriptor) ||
        literal_value.isSqlNull() ||
        literal_value.state != EngineValueState::value ||
        !literal_value.binary_value.empty()) {
      result.diagnostic =
          ViewDiagnostic("relation_projection_view_output_contract_invalid");
      return result;
    }
    parsed_literal = ParseCanonicalI32(literal_value.encoded_value);
    if (!parsed_literal) {
      result.diagnostic =
          ViewDiagnostic("relation_projection_view_literal_int32_invalid");
      return result;
    }
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
  const auto expected_source_resource_epoch = ParseCanonicalU64(
      SingleOptionValue(request, "source_resource_epoch:"));
  if (relation.relation_uuid.canonical != source_uuid ||
      relation.relation_kind != "table" ||
      relation.descriptor_uuid.canonical !=
          SingleOptionValue(request,
                            "source_relation_descriptor_uuid:") ||
      !expected_source_generation || *expected_source_generation == 0 ||
      relation.descriptor_generation != *expected_source_generation ||
      !expected_source_resource_epoch ||
      *expected_source_resource_epoch == 0 ||
      request.context.resource_epoch != *expected_source_resource_epoch) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_source_descriptor_stale");
    return result;
  }
  bool duplicate_column = false;
  const auto* source_column = FindExactColumn(
      relation,
      source_request.requested_column_uuid.canonical,
      &duplicate_column);
  if (duplicate_column || source_column == nullptr ||
      dt::CanonicalTypeIdFromStableName(
          source_column->value_descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int32 ||
      source_column->value_descriptor.descriptor_uuid.canonical.empty() ||
      !DescriptorExactlyMatches(source_request.descriptor,
                                source_column->value_descriptor) ||
      source_request.nullable != source_column->nullable) {
    result.diagnostic = ViewDiagnostic(
        duplicate_column
            ? "relation_projection_view_source_column_ambiguous"
            : "relation_projection_view_source_column_descriptor_stale");
    return result;
  }

  EngineApiRequest resolve = request;
  resolve.target_object.uuid.canonical.clear();
  resolve.option_envelopes.clear();
  resolve.sql_object_reference.expected_object_type = "view";
  resolve.sql_object_reference.object_name.raw_text = view_name;
  resolve.sql_object_reference.object_name.identifier_profile_uuid =
      request.context.identifier_profile_uuid;
  const auto by_name = ResolveNameRegistryPrivate(resolve, "view");
  if (by_name.ok) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_already_exists");
    return result;
  }
  if (by_name.diagnostic.code != "CATALOG.NAME.NOT_FOUND") {
    result.diagnostic = by_name.diagnostic;
    return result;
  }

  std::set<std::string> identities = {
      relation.relation_uuid.canonical,
      relation.descriptor_uuid.canonical,
      source_column->column_uuid.canonical,
      source_column->value_descriptor.descriptor_uuid.canonical};
  const auto visible_inventory =
      LoadVisibleIdentityInventory(request.context);
  if (!visible_inventory.ok) {
    result.diagnostic = visible_inventory.diagnostic;
    return result;
  }
  const auto view_uuid =
      AllocateDistinctObjectUuid(visible_inventory, &identities);
  const auto view_descriptor_uuid =
      AllocateDistinctObjectUuid(visible_inventory, &identities);
  if (!view_uuid || !view_descriptor_uuid) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_uuid_allocation_failed");
    return result;
  }

  EngineRelationProjectionViewDescriptor descriptor;
  descriptor.present = true;
  descriptor.marker = marker;
  descriptor.view_uuid.canonical = *view_uuid;
  descriptor.view_descriptor_uuid.canonical = *view_descriptor_uuid;
  descriptor.view_descriptor_generation = 1;
  descriptor.source_relation_uuid = relation.relation_uuid;
  descriptor.source_relation_descriptor_uuid = relation.descriptor_uuid;
  descriptor.source_relation_descriptor_generation =
      relation.descriptor_generation;
  descriptor.source_resource_epoch = *expected_source_resource_epoch;

  EngineRelationProjectionViewOutput source_output;
  source_output.ordinal = 0;
  const auto source_output_uuid =
      AllocateDistinctObjectUuid(visible_inventory, &identities);
  const auto source_expression_uuid =
      AllocateDistinctObjectUuid(visible_inventory, &identities);
  source_output.output_type =
      AllocateOutputType(source_column->value_descriptor,
                         visible_inventory,
                         &identities);
  if (!source_output_uuid || !source_expression_uuid ||
      source_output.output_type.type_descriptor_uuid.canonical.empty()) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_output_uuid_allocation_failed");
    return result;
  }
  source_output.output_column_uuid.canonical = *source_output_uuid;
  source_output.expression_uuid.canonical = *source_expression_uuid;
  source_output.output_name = source_output_name;
  source_output.nullable = source_column->nullable;
  source_output.expression_kind =
      EngineRelationProjectionExpressionKind::source_column;
  source_output.source_column_uuid = source_column->column_uuid;
  source_output.source_column_type_descriptor_uuid =
      source_column->value_descriptor.descriptor_uuid;

  descriptor.outputs.push_back(std::move(source_output));
  if (v1) {
    EngineRelationProjectionViewOutput literal_output;
    literal_output.ordinal = 1;
    const auto literal_output_uuid =
        AllocateDistinctObjectUuid(visible_inventory, &identities);
    const auto literal_expression_uuid =
        AllocateDistinctObjectUuid(visible_inventory, &identities);
    literal_output.output_type = AllocateOutputType(
        EngineRelationProjectionInt32LiteralInputDescriptor(),
        visible_inventory,
        &identities);
    if (!literal_output_uuid || !literal_expression_uuid ||
        literal_output.output_type.type_descriptor_uuid.canonical.empty()) {
      result.diagnostic = ViewDiagnostic(
          "relation_projection_view_output_uuid_allocation_failed");
      return result;
    }
    literal_output.output_column_uuid.canonical = *literal_output_uuid;
    literal_output.expression_uuid.canonical = *literal_expression_uuid;
    literal_output.output_name = literal_output_name;
    literal_output.nullable = false;
    literal_output.expression_kind =
        EngineRelationProjectionExpressionKind::typed_int32_literal;
    literal_output.literal_int32 = *parsed_literal;
    descriptor.outputs.push_back(std::move(literal_output));
  }
  descriptor.diagnostic = OkDiagnostic();

  EngineRelationProjectionEnvelope envelope;
  envelope.marker = descriptor.marker;
  envelope.relation_uuid = descriptor.source_relation_uuid;
  envelope.relation_descriptor_uuid =
      descriptor.source_relation_descriptor_uuid;
  envelope.relation_descriptor_generation =
      descriptor.source_relation_descriptor_generation;
  envelope.source_resource_epoch = descriptor.source_resource_epoch;
  envelope.outputs = descriptor.outputs;
  const auto envelope_validation =
      ValidateEngineRelationProjectionEnvelope(envelope);
  if (envelope_validation.error) {
    result.diagnostic = envelope_validation;
    return result;
  }
  const auto binding =
      BindEngineRelationProjectionEnvelope(envelope, relation);
  if (!binding.ok) {
    result.diagnostic = binding.diagnostic;
    return result;
  }

  result.canonical_persisted_options = PersistedOptions(descriptor);
  const std::size_t expected_persisted_option_count =
      descriptor.marker == kEngineRelationProjectionViewMarkerV1
          ? kPersistedOptionCountV1
          : kPersistedOptionCountV2;
  if (result.canonical_persisted_options.size() !=
      expected_persisted_option_count) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_view_persisted_codec_invalid");
    return result;
  }
  result.descriptor = std::move(descriptor);
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineRelationProjectionViewDescriptor DescribeEngineRelationProjectionView(
    const EngineRequestContext& context,
    const std::string& view_uuid) {
  EngineRelationProjectionViewDescriptor descriptor;
  descriptor.diagnostic = OkDiagnostic();
  if (view_uuid.empty()) {
    descriptor.diagnostic =
        ViewDiagnostic("relation_projection_view_uuid_required");
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
  if (!lookup.record) return descriptor;

  const auto& record = *lookup.record;
  const auto options = PayloadOptions(record.payload);
  if (!PayloadMentionsRelationProjectionViewFamily(options)) {
    if (record.payload.find("engine.relation_projection_view") !=
        std::string::npos) {
      descriptor.diagnostic =
          ViewDiagnostic("relation_projection_view_descriptor_invalid");
    }
    return descriptor;
  }
  const auto parsed = ParsePersistedDescriptor(record);
  if (!PayloadRequestsRelationProjectionView(options) || !parsed ||
      record.object_kind != "view" ||
      record.operation_id != "ddl.create_view" ||
      record.state != "created" || record.deleted) {
    descriptor.diagnostic =
        ViewDiagnostic("relation_projection_view_descriptor_invalid");
    return descriptor;
  }
  return *parsed;
}

EngineDescriptor EngineRelationProjectionViewSemanticDescriptor(
    const EngineRelationProjectionViewDescriptor& descriptor) {
  EngineDescriptor semantic;
  const bool v1 = descriptor.marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = descriptor.marker == kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if (!descriptor.present || descriptor.view_uuid.canonical.empty() ||
      descriptor.view_descriptor_uuid.canonical.empty() ||
      descriptor.view_descriptor_generation == 0 ||
      (!v1 && !v2) ||
      descriptor.outputs.size() != expected_output_count) {
    return semantic;
  }
  semantic.descriptor_uuid = descriptor.view_descriptor_uuid;
  semantic.descriptor_kind = "relation_projection_view";
  semantic.canonical_type_name = descriptor.marker;
  semantic.encoded_descriptor =
      std::string("marker=") + descriptor.marker +
      ";view_uuid=" + descriptor.view_uuid.canonical +
      ";view_descriptor_generation=" +
      std::to_string(descriptor.view_descriptor_generation) +
      ";output_count=" + std::to_string(expected_output_count);
  return semantic;
}

std::vector<EngineRelationProjectionViewSemanticOutput>
EngineRelationProjectionViewSemanticOutputs(
    const EngineRelationProjectionViewDescriptor& descriptor) {
  std::vector<EngineRelationProjectionViewSemanticOutput> outputs;
  const bool v1 = descriptor.marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = descriptor.marker == kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if (!descriptor.present || (!v1 && !v2) ||
      descriptor.outputs.size() != expected_output_count) {
    return outputs;
  }
  outputs.reserve(descriptor.outputs.size());
  for (const auto& persisted : descriptor.outputs) {
    EngineRelationProjectionViewSemanticOutput output;
    output.ordinal = persisted.ordinal;
    output.output_column_uuid = persisted.output_column_uuid;
    output.output_name = persisted.output_name;
    output.output_type = persisted.output_type;
    output.nullable = persisted.nullable;
    outputs.push_back(std::move(output));
  }
  return outputs;
}

bool IsEngineRelationProjectionViewSelectRequest(
    const EngineSelectRowsRequest& request) {
  return request.source_object.object_kind == "view" &&
         request.relation_projection_view.present;
}

EngineApiDiagnostic ExpandEngineRelationProjectionViewSelect(
    const EngineSelectRowsRequest& request,
    EngineSelectRowsRequest* expanded,
    EngineRelationProjectionViewDescriptor* descriptor_out) {
  if (expanded == nullptr || descriptor_out == nullptr) {
    return ViewDiagnostic(
        "relation_projection_view_expansion_output_required");
  }
  const auto& semantic = request.relation_projection_view;
  if (!semantic.present ||
      semantic.marker != kEngineRelationProjectionViewMarkerV1 ||
      semantic.view_uuid.canonical != request.source_object.uuid.canonical ||
      semantic.view_descriptor_uuid.canonical.empty() ||
      semantic.view_descriptor_generation == 0 ||
      semantic.outputs.size() != 2 ||
      !request.relation_projection.outputs.empty() ||
      !request.global_aggregate_projection.outputs.empty() ||
      !request.select_projection.canonical_projection_envelopes.empty() ||
      !request.projection.canonical_projection_envelopes.empty() ||
      !request.select_predicate.predicate_kind.empty() ||
      !request.predicate.predicate_kind.empty() ||
      !request.select_ordering.canonical_ordering_envelopes.empty() ||
      !request.ordering.canonical_ordering_envelopes.empty() ||
      request.limit != 0 || request.offset != 0 ||
      !request.option_envelopes.empty() || !request.rows.empty() ||
      !request.assignments.empty() || !request.descriptors.empty() ||
      !request.columns.empty() || !request.related_objects.empty()) {
    return ViewDiagnostic(
        "relation_projection_view_select_shape_invalid");
  }

  auto descriptor = DescribeEngineRelationProjectionView(
      request.context, request.source_object.uuid.canonical);
  if (descriptor.diagnostic.error) return descriptor.diagnostic;
  if (!descriptor.present) {
    return ViewDiagnostic(
        "relation_projection_view_descriptor_required");
  }
  if (semantic.view_descriptor_uuid.canonical !=
          descriptor.view_descriptor_uuid.canonical ||
      semantic.view_descriptor_generation !=
          descriptor.view_descriptor_generation ||
      semantic.outputs.size() != descriptor.outputs.size()) {
    return ViewDiagnostic(
        "relation_projection_view_semantic_descriptor_stale");
  }
  for (std::size_t i = 0; i < semantic.outputs.size(); ++i) {
    if (!SemanticOutputExactlyMatches(semantic.outputs[i],
                                      descriptor.outputs[i])) {
      return ViewDiagnostic(
          "relation_projection_view_semantic_descriptor_stale");
    }
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
          descriptor.source_relation_descriptor_generation ||
      request.context.resource_epoch == 0 ||
      request.context.resource_epoch != descriptor.source_resource_epoch) {
    return ViewDiagnostic(
        "relation_projection_view_source_descriptor_stale");
  }

  EngineRelationProjectionEnvelope projection;
  projection.marker = descriptor.marker;
  projection.relation_uuid = relation.relation_uuid;
  projection.relation_descriptor_uuid = relation.descriptor_uuid;
  projection.relation_descriptor_generation = relation.descriptor_generation;
  projection.source_resource_epoch = descriptor.source_resource_epoch;
  projection.outputs = descriptor.outputs;
  const auto binding =
      BindEngineRelationProjectionEnvelope(projection, relation);
  if (!binding.ok) return binding.diagnostic;

  EngineSelectRowsRequest next;
  next.context = request.context;
  next.source_object.uuid = relation.relation_uuid;
  next.source_object.object_kind = "table";
  next.relation_projection = std::move(projection);
  *expanded = std::move(next);
  *descriptor_out = std::move(descriptor);
  return OkDiagnostic();
}

bool IsEngineRelationProjectionViewDeleteRequest(
    const EngineDeleteRowsRequest& request) {
  return request.relation_projection_view.present;
}

EngineApiDiagnostic ExpandEngineRelationProjectionViewDelete(
    const EngineDeleteRowsRequest& request,
    EngineDeleteRowsRequest* expanded,
    EngineRelationProjectionViewDescriptor* descriptor_out) {
  if (expanded == nullptr || descriptor_out == nullptr) {
    return ViewDiagnostic(
        "relation_projection_view_delete_expansion_output_required");
  }
  const auto exact_transaction =
      ValidateExactActiveTransaction(request.context, true);
  if (exact_transaction.error) return exact_transaction;
  const auto& semantic = request.relation_projection_view;
  const bool target_object_empty =
      request.target_object.uuid.canonical.empty() &&
      request.target_object.object_kind.empty();
  const bool target_object_matches =
      request.target_object.uuid.canonical ==
          request.target_table.uuid.canonical &&
      request.target_object.object_kind == "view";
  const std::vector<std::string> default_write_options = {
      "result_payload_policy:summary_only",
      "sblr.default_write_result_policy:summary_only"};
  const bool options_exact = request.option_envelopes.empty() ||
                             request.option_envelopes ==
                                 default_write_options;
  const bool base_shape_exact =
      request.target_database.uuid.canonical.empty() &&
      request.target_database.object_kind.empty() &&
      request.target_schema.uuid.canonical.empty() &&
      request.target_schema.object_kind.empty() &&
      (target_object_empty || target_object_matches) &&
      request.related_objects.empty() && request.localized_names.empty() &&
      request.sql_object_reference.expected_object_type.empty() &&
      request.sql_object_reference.path_type == "unqualified" &&
      !request.sql_object_reference.no_search_path &&
      request.sql_object_reference.path_components.empty() &&
      request.sql_object_reference.object_name.raw_text.empty() &&
      !request.sql_object_reference.object_name.was_quoted &&
      request.sql_object_reference.object_name.quote_style.empty() &&
      request.sql_object_reference.object_name.identifier_profile_uuid.empty() &&
      request.sql_object_reference.object_name.normalized_lookup_key.empty() &&
      request.sql_object_reference.object_name.exact_lookup_key.empty() &&
      !request.sql_object_reference.object_name.requires_exact_match &&
      request.sql_object_reference.object_name.source_span.empty() &&
      request.bound_object_identity.object_uuid.canonical.empty() &&
      request.bound_object_identity.resolved_object_type.empty() &&
      request.bound_object_identity.resolved_schema_uuid.canonical.empty() &&
      request.bound_object_identity.parent_object_uuid.canonical.empty() &&
      request.bound_object_identity.catalog_generation_id == 0 &&
      request.bound_object_identity.security_epoch == 0 &&
      request.bound_object_identity.resource_epoch == 0 &&
      request.descriptors.empty() && request.columns.empty() &&
      request.constraints.empty() && request.indexes.empty() &&
      !request.native_row_packet.present &&
      request.native_row_packet.version == 0 &&
      request.native_row_packet.row_count == 0 &&
      request.native_row_packet.column_count == 0 &&
      request.native_row_packet.packet_bytes.empty() &&
      request.native_row_packet.field_order.empty() &&
      request.native_row_packet.column_type_tags.empty() &&
      request.native_row_packet.row_offsets.empty() &&
      request.native_row_packet.row_sizes.empty() && request.rows.empty() &&
      request.shared_row_field_order.empty() && request.assignments.empty() &&
      request.predicate.predicate_kind.empty() &&
      request.predicate.canonical_predicate_envelope.empty() &&
      request.predicate.bound_values.empty() &&
      request.projection.canonical_projection_envelopes.empty() &&
      request.ordering.canonical_ordering_envelopes.empty() &&
      request.physical_profile.names.empty() &&
      request.physical_profile.encoded_profiles.empty() &&
      request.policy_profile.names.empty() &&
      request.policy_profile.encoded_profiles.empty() &&
      request.compatibility_profile.names.empty() &&
      request.compatibility_profile.encoded_profiles.empty() &&
      request.diagnostic_options.empty() && options_exact;
  if (!semantic.present ||
      semantic.marker != kEngineRelationProjectionViewMarkerV2 ||
      semantic.view_descriptor_uuid.canonical.empty() ||
      semantic.view_descriptor_generation == 0 ||
      semantic.outputs.size() != 1 ||
      request.target_table.object_kind != "view" ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          request.target_table.uuid.canonical) ||
      (!request.operation_id.empty() &&
       request.operation_id != "dml.delete_rows") ||
      !base_shape_exact ||
      request.delete_surface_variant != kEngineRelationProjectionViewMarkerV2 ||
      !request.batch_on_column.empty() || request.batch_limit_rows != 0 ||
      request.limit != 0 || request.offset != 0 ||
      !request.series_name.empty() || !request.tombstone_only ||
      request.delete_predicate.predicate_kind != "column_equals" ||
      request.delete_predicate.canonical_predicate_envelope.empty() ||
      request.delete_predicate.bound_values.size() != 1) {
    return ViewDiagnostic(
        "relation_projection_view_delete_shape_invalid");
  }

  const auto& predicate_value = request.delete_predicate.bound_values.front();
  std::int32_t canonical_predicate = 0;
  const auto parsed_predicate =
      ParseCanonicalI32(predicate_value.encoded_value);
  if (!predicate_value.descriptor.descriptor_uuid.canonical.empty() ||
      predicate_value.descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          predicate_value.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int32 ||
      predicate_value.descriptor.encoded_descriptor != "type=int32" ||
      predicate_value.state != EngineValueState::value ||
      predicate_value.isSqlNull() || !predicate_value.binary_value.empty() ||
      !parsed_predicate) {
    return ViewDiagnostic(
        "relation_projection_view_delete_predicate_invalid");
  }
  canonical_predicate = *parsed_predicate;

  auto descriptor = DescribeEngineRelationProjectionView(
      request.context, request.target_table.uuid.canonical);
  if (descriptor.diagnostic.error) return descriptor.diagnostic;
  if (!descriptor.present ||
      descriptor.marker != kEngineRelationProjectionViewMarkerV2 ||
      descriptor.outputs.size() != 1) {
    return ViewDiagnostic(
        "relation_projection_view_delete_descriptor_required");
  }
  if (semantic.view_descriptor_uuid.canonical !=
          descriptor.view_descriptor_uuid.canonical ||
      semantic.view_descriptor_generation !=
          descriptor.view_descriptor_generation ||
      !SemanticOutputExactlyMatches(semantic.outputs.front(),
                                    descriptor.outputs.front()) ||
      request.delete_predicate.canonical_predicate_envelope !=
          descriptor.outputs.front().output_name) {
    return ViewDiagnostic(
        "relation_projection_view_delete_semantic_descriptor_stale");
  }

  const auto source = LoadMgaRelationStorageDescriptor(
      request.context, descriptor.source_relation_uuid.canonical);
  if (!source.ok) return source.diagnostic;
  const auto& relation = source.descriptor;
  if (relation.relation_kind != "table" ||
      relation.relation_uuid.canonical !=
          descriptor.source_relation_uuid.canonical ||
      relation.descriptor_uuid.canonical !=
          descriptor.source_relation_descriptor_uuid.canonical ||
      relation.descriptor_generation !=
          descriptor.source_relation_descriptor_generation ||
      request.context.resource_epoch == 0 ||
      request.context.resource_epoch != descriptor.source_resource_epoch) {
    return ViewDiagnostic(
        "relation_projection_view_delete_source_descriptor_stale");
  }

  EngineRelationProjectionEnvelope projection;
  projection.marker = descriptor.marker;
  projection.relation_uuid = relation.relation_uuid;
  projection.relation_descriptor_uuid = relation.descriptor_uuid;
  projection.relation_descriptor_generation = relation.descriptor_generation;
  projection.source_resource_epoch = descriptor.source_resource_epoch;
  projection.outputs = descriptor.outputs;
  const auto binding =
      BindEngineRelationProjectionEnvelope(projection, relation);
  if (!binding.ok || binding.outputs.size() != 1 ||
      binding.outputs.front().source_column_name_key.empty()) {
    return binding.diagnostic.error
               ? binding.diagnostic
               : ViewDiagnostic(
                     "relation_projection_view_delete_source_binding_invalid");
  }
  bool duplicate_source_column = false;
  const auto* source_column = FindExactColumn(
      relation,
      descriptor.outputs.front().source_column_uuid.canonical,
      &duplicate_source_column);
  if (duplicate_source_column || source_column == nullptr) {
    return ViewDiagnostic(
        "relation_projection_view_delete_source_binding_invalid");
  }

  EngineDeleteRowsRequest next;
  next.context = request.context;
  next.operation_id = "dml.delete_rows";
  next.target_table.uuid = relation.relation_uuid;
  next.target_table.object_kind = "table";
  next.delete_predicate = request.delete_predicate;
  next.delete_predicate.canonical_predicate_envelope =
      binding.outputs.front().source_column_name_key;
  next.delete_predicate.bound_values.front().descriptor =
      source_column->value_descriptor;
  next.delete_predicate.bound_values.front().encoded_value =
      std::to_string(canonical_predicate);
  next.delete_predicate.bound_values.front().setState(EngineValueState::value);
  next.delete_surface_variant = "delete";
  next.tombstone_only = true;
  next.option_envelopes = request.option_envelopes;
  *expanded = std::move(next);
  *descriptor_out = std::move(descriptor);
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateEngineRelationProjectionEnvelope(
    const EngineRelationProjectionEnvelope& envelope) {
  const bool v1 = envelope.marker == kEngineRelationProjectionViewMarkerV1;
  const bool v2 = envelope.marker == kEngineRelationProjectionViewMarkerV2;
  const std::size_t expected_output_count = v1 ? 2u : 1u;
  if (!CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          envelope.relation_uuid.canonical) ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          envelope.relation_descriptor_uuid.canonical) ||
      envelope.relation_descriptor_generation == 0 ||
      envelope.source_resource_epoch == 0 ||
      (!v1 && !v2) ||
      envelope.outputs.size() != expected_output_count) {
    return ViewDiagnostic(
        "relation_projection_envelope_shape_invalid");
  }
  const auto& source = envelope.outputs[0];
  const auto valid_common = [](const EngineRelationProjectionViewOutput& output,
                               std::uint32_t ordinal) {
    return output.ordinal == ordinal &&
           CanonicalTypedUuid(
               scratchbird::core::platform::UuidKind::object,
               output.output_column_uuid.canonical) &&
           CanonicalTypedUuid(
               scratchbird::core::platform::UuidKind::object,
               output.expression_uuid.canonical) &&
           CanonicalTypedUuid(
               scratchbird::core::platform::UuidKind::object,
               output.output_type.type_descriptor_uuid.canonical) &&
           SafeUnquotedIdentifier(output.output_name) &&
           !output.output_type.descriptor_kind.empty() &&
           !output.output_type.canonical_type_name.empty() &&
           !output.output_type.encoded_descriptor.empty() &&
           dt::CanonicalTypeIdFromStableName(
               output.output_type.canonical_type_name) ==
               dt::CanonicalTypeId::int32;
  };
  if (!valid_common(source, 0) ||
      source.expression_kind !=
          EngineRelationProjectionExpressionKind::source_column ||
      !CanonicalTypedUuid(scratchbird::core::platform::UuidKind::object,
                          source.source_column_uuid.canonical) ||
      !CanonicalTypedUuid(
          scratchbird::core::platform::UuidKind::object,
          source.source_column_type_descriptor_uuid.canonical)) {
    return ViewDiagnostic(
        "relation_projection_output_contract_invalid");
  }
  if (v1) {
    const auto& literal = envelope.outputs[1];
    if (!valid_common(literal, 1) ||
        LowerAscii(source.output_name) == LowerAscii(literal.output_name) ||
        literal.expression_kind !=
            EngineRelationProjectionExpressionKind::typed_int32_literal ||
        !literal.source_column_uuid.canonical.empty() ||
        !literal.source_column_type_descriptor_uuid.canonical.empty() ||
        literal.nullable ||
        !DescriptorTypeShapeMatches(
            literal.output_type,
            EngineRelationProjectionInt32LiteralInputDescriptor())) {
      return ViewDiagnostic(
          "relation_projection_output_contract_invalid");
    }
  }

  std::set<std::string> identities = {
      envelope.relation_uuid.canonical,
      envelope.relation_descriptor_uuid.canonical,
      source.output_column_uuid.canonical,
      source.expression_uuid.canonical,
      source.output_type.type_descriptor_uuid.canonical,
      source.source_column_uuid.canonical};
  if (v1) {
    const auto& literal = envelope.outputs[1];
    identities.insert(literal.output_column_uuid.canonical);
    identities.insert(literal.expression_uuid.canonical);
    identities.insert(literal.output_type.type_descriptor_uuid.canonical);
  }
  const bool source_type_identity_conflicts =
      source.source_column_type_descriptor_uuid.canonical !=
          source.source_column_uuid.canonical &&
      identities.contains(
          source.source_column_type_descriptor_uuid.canonical);
  if (source_type_identity_conflicts ||
      identities.size() != (v1 ? 9u : 6u)) {
    return ViewDiagnostic(
        "relation_projection_identity_collision");
  }
  return OkDiagnostic();
}

EngineRelationProjectionBindingResult BindEngineRelationProjectionEnvelope(
    const EngineRelationProjectionEnvelope& envelope,
    const MgaRelationStorageDescriptor& relation_descriptor) {
  EngineRelationProjectionBindingResult result;
  result.diagnostic = OkDiagnostic();
  const auto validated = ValidateEngineRelationProjectionEnvelope(envelope);
  if (validated.error) {
    result.diagnostic = validated;
    return result;
  }
  if (relation_descriptor.relation_kind != "table" ||
      relation_descriptor.relation_uuid.canonical !=
          envelope.relation_uuid.canonical ||
      relation_descriptor.descriptor_uuid.canonical !=
          envelope.relation_descriptor_uuid.canonical ||
      relation_descriptor.descriptor_generation !=
          envelope.relation_descriptor_generation) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_source_descriptor_stale");
    return result;
  }

  bool duplicate_column = false;
  const auto* source_column = FindExactColumn(
      relation_descriptor,
      envelope.outputs[0].source_column_uuid.canonical,
      &duplicate_column);
  if (duplicate_column || source_column == nullptr ||
      source_column->canonical_name_key.empty() ||
      dt::CanonicalTypeIdFromStableName(
          source_column->value_descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int32 ||
      source_column->value_descriptor.descriptor_uuid.canonical !=
          envelope.outputs[0]
              .source_column_type_descriptor_uuid.canonical ||
      !DescriptorTypeShapeMatches(envelope.outputs[0].output_type,
                                  source_column->value_descriptor) ||
      envelope.outputs[0].nullable != source_column->nullable) {
    result.diagnostic = ViewDiagnostic(
        duplicate_column
            ? "relation_projection_source_column_ambiguous"
            : "relation_projection_source_column_descriptor_stale");
    return result;
  }

  EngineBoundRelationProjectionOutput source;
  source.output = envelope.outputs[0];
  source.source_column_name_key = source_column->canonical_name_key;
  result.outputs.push_back(std::move(source));
  if (envelope.marker == kEngineRelationProjectionViewMarkerV1) {
    EngineBoundRelationProjectionOutput literal;
    literal.output = envelope.outputs[1];
    result.outputs.push_back(std::move(literal));
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineRelationProjectionExecutionResult ExecuteEngineRelationProjection(
    const std::vector<EngineBoundRelationProjectionOutput>& outputs,
    const MgaRelationStorageDescriptor& relation_descriptor,
    std::uint64_t source_resource_epoch,
    const std::vector<CrudRowVersionRecord>& visible_rows) {
  EngineRelationProjectionExecutionResult result;
  result.diagnostic = OkDiagnostic();
  if (outputs.size() != 2) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_binding_required");
    return result;
  }

  EngineRelationProjectionEnvelope envelope;
  envelope.relation_uuid = relation_descriptor.relation_uuid;
  envelope.relation_descriptor_uuid = relation_descriptor.descriptor_uuid;
  envelope.relation_descriptor_generation =
      relation_descriptor.descriptor_generation;
  envelope.source_resource_epoch = source_resource_epoch;
  envelope.outputs = {outputs[0].output, outputs[1].output};
  const auto rebound =
      BindEngineRelationProjectionEnvelope(envelope, relation_descriptor);
  if (!rebound.ok) {
    result.diagnostic = rebound.diagnostic;
    return result;
  }
  if (rebound.outputs[0].source_column_name_key !=
      outputs[0].source_column_name_key) {
    result.diagnostic =
        ViewDiagnostic("relation_projection_source_binding_stale");
    return result;
  }

  result.result_shape.result_kind = "query_rowset";
  result.result_shape.columns.push_back(
      EngineRelationProjectionOutputTypeDescriptor(
          outputs[0].output.output_type));
  result.result_shape.columns.push_back(
      EngineRelationProjectionOutputTypeDescriptor(
          outputs[1].output.output_type));
  result.result_shape.rows.reserve(visible_rows.size());

  const EngineDescriptor source_type =
      EngineRelationProjectionOutputTypeDescriptor(
          outputs[0].output.output_type);
  const EngineDescriptor literal_type =
      EngineRelationProjectionOutputTypeDescriptor(
          outputs[1].output.output_type);
  for (const auto& row : visible_rows) {
    const auto stored = StoredFieldValueExact(
        row, outputs[0].source_column_name_key);
    if (!stored.found || stored.duplicate) {
      result.result_shape.rows.clear();
      result.scanned_visible_row_count = 0;
      result.diagnostic = ViewDiagnostic(
          stored.duplicate
              ? "relation_projection_source_value_ambiguous"
              : "relation_projection_source_value_missing");
      return result;
    }

    EngineRowValue projected;
    projected.requested_row_uuid.canonical = row.row_uuid;
    EngineTypedValue source_value;
    source_value.descriptor = source_type;
    if (stored.value == "<NULL>") {
      source_value.setState(EngineValueState::sql_null);
    } else {
      std::string canonical;
      if (!CanonicalInt32Value(source_type, stored.value, &canonical)) {
        result.result_shape.rows.clear();
        result.scanned_visible_row_count = 0;
        result.diagnostic = ViewDiagnostic(
            "relation_projection_source_value_invalid");
        return result;
      }
      source_value.encoded_value = std::move(canonical);
      source_value.setState(EngineValueState::value);
    }
    projected.fields.push_back(
        {outputs[0].output.output_name, std::move(source_value)});

    EngineTypedValue literal_value;
    literal_value.descriptor = literal_type;
    literal_value.encoded_value =
        std::to_string(outputs[1].output.literal_int32);
    literal_value.setState(EngineValueState::value);
    projected.fields.push_back(
        {outputs[1].output.output_name, std::move(literal_value)});
    result.result_shape.rows.push_back(std::move(projected));
  }

  result.scanned_visible_row_count = visible_rows.size();
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

}  // namespace scratchbird::engine::internal_api
