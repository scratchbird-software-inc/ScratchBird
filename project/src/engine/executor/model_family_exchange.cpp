// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_exchange.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>
#include <unordered_set>

namespace scratchbird::engine::executor {
namespace {

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

ModelExchangeResultV1 Refuse(const char* diagnostic, std::string detail) {
  ModelExchangeResultV1 result;
  result.diagnostic_id = diagnostic;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

ModelInputValidationResultV1 ValidateModelFamilySourceInputV1(
    const ModelSourceInputDescriptorV1& input) {
  ModelInputValidationResultV1 result;
  const bool document_family = input.family_id == "document";
  const bool graph_family = input.family_id == "graph";
  const bool valid_operation =
      (document_family &&
       (input.operation_id == "DOCUMENT_FIND" ||
        input.operation_id == "DOCUMENT_PATH" ||
        input.operation_id == "DOCUMENT_UNNEST")) ||
      (graph_family &&
       (input.operation_id == "GRAPH_MATCH" ||
        input.operation_id == "GRAPH_EXPAND"));
  if (input.abi_version != 1 ||
      input.input_descriptor_id != "SB_MODEL_SOURCE_INPUT_DESCRIPTOR_V1" ||
      (!document_family && !graph_family) || !valid_operation ||
      (input.operation_id == "DOCUMENT_UNNEST" && !input.object_uuid.empty()) ||
      (input.operation_id != "DOCUMENT_UNNEST" &&
       !CanonicalUuid(input.object_uuid)) ||
      input.physical_node_id == 0 || input.causal_counter_id == 0 ||
      input.provider_generation == 0 || input.catalog_generation == 0 ||
      input.descriptor_generation == 0 || input.security_generation == 0 ||
      input.policy_generation == 0 || input.resource_generation == 0 ||
      input.output_descriptor_ids.empty() || input.maximum_rows == 0 ||
      input.maximum_cells == 0 || input.maximum_memory_bytes == 0 ||
      !CanonicalUuid(input.selected_alternative_uuid) ||
      !CanonicalUuid(input.capability_uuid) ||
      !CanonicalUuid(input.provider_uuid) ||
      !CanonicalUuid(input.result_handle_uuid) ||
      !CanonicalUuid(input.catalog_epoch_uuid) ||
      !CanonicalUuid(input.security_context_uuid) ||
      !CanonicalUuid(input.policy_snapshot_uuid) ||
      !CanonicalUuid(input.resource_contract_uuid) ||
      !PhysicalMgaStatementContextValid(input.mga_statement_context) ||
      input.parser_execution_authority_claimed ||
      input.transaction_finality_authority_claimed) {
    result.diagnostic_id = kModelTypedExchangeInvalid;
    result.detail = "model source input descriptor is incomplete";
    return result;
  }
  result.accepted = true;
  return result;
}

ModelExchangeResultV1 PublishModelFamilyExchangeV1(
    const ModelSourceInputDescriptorV1& input,
    const ModelProviderBatchV1& provider_batch) {
  // QOW-SOURCE-CES05-MODEL-TYPED-EXCHANGE-V1
  const auto input_validation = ValidateModelFamilySourceInputV1(input);
  if (!input_validation.accepted) {
    return Refuse(input_validation.diagnostic_id.c_str(),
                  input_validation.detail);
  }
  if (provider_batch.abi_version != 1) {
    return Refuse(kModelTypedExchangeInvalid,
                  "model provider batch version is unsupported");
  }
  if (provider_batch.provider_uuid != input.provider_uuid ||
      provider_batch.provider_generation != input.provider_generation ||
      provider_batch.result_handle_uuid != input.result_handle_uuid ||
      provider_batch.causal_counter_id != input.causal_counter_id ||
      !PhysicalMgaStatementContextEqual(provider_batch.mga_statement_context,
                                        input.mga_statement_context) ||
      !CanonicalUuid(provider_batch.security_receipt_uuid) ||
      provider_batch.provider_visibility_authority_claimed ||
      provider_batch.provider_finality_authority_claimed) {
    return Refuse(kModelMgaContextMismatch,
                  "provider exchange identity or MGA context was substituted");
  }
  if (provider_batch.output_descriptor_ids != input.output_descriptor_ids) {
    return Refuse(kModelTypedExchangeInvalid,
                  "provider output descriptors differ from the bound input");
  }
  const bool graph_family = input.family_id == "graph";
  if (graph_family) {
    std::size_t preflight_cell_count = 0;
    // The provider batch is caller/provider-owned. The grant covers every
    // allocation copied into the engine-owned output descriptor at the same
    // time as the normalized batch.
    std::uint64_t preflight_memory_bytes =
        sizeof(ModelSourceOutputDescriptorV1);
    const auto preflight_account = [&](const std::uint64_t bytes) {
      if (bytes > std::numeric_limits<std::uint64_t>::max() -
                      preflight_memory_bytes) {
        return false;
      }
      preflight_memory_bytes += bytes;
      return preflight_memory_bytes <= input.maximum_memory_bytes;
    };
    const auto preflight_string = [&](const std::string_view value) {
      return preflight_account(value.size());
    };
    if (provider_batch.batch.rows.size() > input.maximum_rows) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange row preflight exceeded its resource contract");
    }
    if (provider_batch.ordered_row_identities.size() !=
        provider_batch.batch.rows.size()) {
      return Refuse(kModelTypedExchangeInvalid,
                    "graph ordered row identity cardinality is incomplete");
    }
    if (!preflight_string("SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1") ||
        !preflight_string("SB_MODEL_PROPERTY_DESCRIPTOR_V1") ||
        !preflight_string(input.input_descriptor_id) ||
        !preflight_string(input.family_id) ||
        !preflight_string(input.operation_id) ||
        !preflight_string(input.object_uuid) ||
        !preflight_string(input.selected_alternative_uuid) ||
        !preflight_string(input.capability_uuid) ||
        !preflight_string(input.provider_uuid) ||
        !preflight_string(input.result_handle_uuid) ||
        !preflight_account(input.output_descriptor_ids.size() *
                           sizeof(std::uint32_t)) ||
        !preflight_string(provider_batch.properties.property_descriptor_id) ||
        !preflight_string(provider_batch.properties.property_uuid) ||
        !preflight_string(provider_batch.properties.ordering_id) ||
        !preflight_string(provider_batch.properties.partitioning_id) ||
        !preflight_string(provider_batch.properties.uniqueness_id) ||
        !preflight_string(input.mga_statement_context.statement_uuid) ||
        !preflight_string(input.mga_statement_context.owning_transaction_uuid) ||
        !preflight_string(input.mga_statement_context.statement_snapshot_uuid) ||
        !preflight_string(
            input.mga_statement_context.statement_metadata_snapshot_uuid) ||
        !preflight_string(input.mga_statement_context.snapshot_kind) ||
        !preflight_account(
            input.mga_statement_context.active_excluded_local_transaction_ids
                .size() * sizeof(std::uint64_t)) ||
        !preflight_account(
            input.mga_statement_context.in_doubt_excluded_local_transaction_ids
                .size() * sizeof(std::uint64_t)) ||
        !preflight_string(provider_batch.security_receipt_uuid)) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange output metadata exceeded its resource contract");
    }
    for (const auto& identity : provider_batch.ordered_row_identities) {
      if (!preflight_account(sizeof(ModelProviderRowIdentityV1)) ||
          !preflight_string(identity.document_uuid) ||
          !preflight_string(identity.row_uuid) ||
          !preflight_string(identity.vertex_uuid) ||
          !preflight_string(identity.edge_uuid) ||
          !preflight_string(identity.path_uuid)) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange identity preflight exceeded its resource contract");
      }
    }
    for (const auto& column : provider_batch.batch.columns) {
      if (!preflight_account(sizeof(ExecutorColumnDescriptor)) ||
          !preflight_account(column.stable_name.size()) ||
          !preflight_account(
              column.descriptor.descriptor_uuid.canonical.size()) ||
          !preflight_account(column.descriptor.descriptor_kind.size()) ||
          !preflight_account(column.descriptor.canonical_type_name.size()) ||
          !preflight_account(column.descriptor.encoded_descriptor.size())) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange descriptor preflight exceeded its resource contract");
      }
    }
    for (const auto& row : provider_batch.batch.rows) {
      if (preflight_cell_count > input.maximum_cells ||
          row.values.size() > input.maximum_cells - preflight_cell_count ||
          !preflight_account(sizeof(DescriptorTuple))) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph exchange row preflight exceeded its resource contract");
      }
      preflight_cell_count += row.values.size();
      for (const auto& value : row.values) {
        if (!preflight_account(sizeof(internal_api::EngineTypedValue)) ||
            !preflight_account(
                value.descriptor.descriptor_uuid.canonical.size()) ||
            !preflight_account(value.descriptor.descriptor_kind.size()) ||
            !preflight_account(value.descriptor.canonical_type_name.size()) ||
            !preflight_account(value.descriptor.encoded_descriptor.size()) ||
            !preflight_account(value.encoded_value.size()) ||
            !preflight_account(value.binary_value.size())) {
          return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "graph exchange value preflight exceeded its resource contract");
        }
      }
    }
    if (preflight_cell_count > input.maximum_cells) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph exchange cell preflight exceeded its resource contract");
    }
  }
  if (provider_batch.ordered_row_identities.size() !=
      provider_batch.batch.rows.size()) {
    return Refuse(kModelTypedExchangeInvalid,
                  "document ordered row identity cardinality is incomplete");
  }
  if (graph_family) {
    std::uint64_t uniqueness_peak =
        3 * sizeof(std::unordered_set<std::string>);
    for (const auto& identity : provider_batch.ordered_row_identities) {
      constexpr std::uint64_t kSetNodeOverhead =
          sizeof(std::string) + 4 * sizeof(void*) + 64;
      const auto dynamic = static_cast<std::uint64_t>(
          identity.row_uuid.size() + identity.path_uuid.size());
      if (2 * kSetNodeOverhead >
              std::numeric_limits<std::uint64_t>::max() - uniqueness_peak ||
          dynamic > std::numeric_limits<std::uint64_t>::max() -
                        uniqueness_peak - 2 * kSetNodeOverhead) {
        return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "graph uniqueness preflight overflowed");
      }
      uniqueness_peak += 2 * kSetNodeOverhead + dynamic;
    }
    if (uniqueness_peak > input.maximum_memory_bytes) {
      return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph uniqueness validation exceeded its resource contract");
    }
  }
  {
    // These temporary uniqueness sets are destroyed before the normalized
    // output batch is copied, keeping their accounted peak disjoint.
    std::unordered_set<std::string> document_uuids;
    std::unordered_set<std::string> row_uuids;
    std::unordered_set<std::string> path_uuids;
    for (const auto& identity : provider_batch.ordered_row_identities) {
      const bool document_identity =
          !graph_family && CanonicalUuid(identity.document_uuid) &&
          CanonicalUuid(identity.row_uuid) &&
          document_uuids.insert(identity.document_uuid).second &&
          row_uuids.insert(identity.row_uuid).second &&
          identity.vertex_uuid.empty() && identity.edge_uuid.empty() &&
          identity.path_uuid.empty() && identity.graph_depth == 0;
      const bool graph_edge_identity =
          (input.operation_id == "GRAPH_MATCH" && identity.graph_depth == 0 &&
           identity.edge_uuid.empty()) ||
          (input.operation_id == "GRAPH_EXPAND" &&
           ((identity.graph_depth == 0 && identity.edge_uuid.empty()) ||
            (identity.graph_depth > 0 && CanonicalUuid(identity.edge_uuid))));
      const bool graph_identity =
          graph_family && identity.document_uuid.empty() &&
          CanonicalUuid(identity.row_uuid) &&
          CanonicalUuid(identity.vertex_uuid) && graph_edge_identity &&
          CanonicalUuid(identity.path_uuid) &&
          row_uuids.insert(identity.row_uuid).second &&
          path_uuids.insert(identity.path_uuid).second;
      if (!document_identity && !graph_identity) {
        return Refuse(
            kModelTypedExchangeInvalid,
            "model-family row uniqueness or ordering identity is invalid");
      }
    }
  }
  if (provider_batch.properties.abi_version != 1 ||
      provider_batch.properties.property_descriptor_id !=
          "SB_MODEL_PROPERTY_DESCRIPTOR_V1" ||
      !CanonicalUuid(provider_batch.properties.property_uuid) ||
      !provider_batch.properties.exact ||
      provider_batch.properties.ordering_id != "fixture_order" ||
      provider_batch.properties.partitioning_id !=
          "single_local_partition" ||
      provider_batch.properties.uniqueness_id !=
          (graph_family ? "path_uuid" : "document_uuid") ||
      !provider_batch.residual_recheck_complete ||
      !provider_batch.base_row_mga_recheck_complete ||
      !provider_batch.security_recheck_complete ||
      provider_batch.properties.residual_recheck_complete !=
          provider_batch.residual_recheck_complete ||
      provider_batch.properties.base_row_mga_recheck_complete !=
          provider_batch.base_row_mga_recheck_complete ||
      provider_batch.properties.security_recheck_complete !=
          provider_batch.security_recheck_complete) {
    return Refuse(kModelTypedExchangeInvalid,
                  "model-family exactness or recheck receipt is incomplete");
  }

  if (graph_family) {
    const auto validation = ValidateCanonicalDescriptorBatch(
        provider_batch.batch, input.output_descriptor_ids);
    if (!validation.ok) {
      return Refuse(kModelTypedExchangeInvalid, validation.detail);
    }
    const auto column_ordinal = [&](const std::string_view name)
        -> std::optional<std::size_t> {
      const auto column = std::ranges::find_if(
          provider_batch.batch.columns,
          [&](const auto& candidate) { return candidate.stable_name == name; });
      return column == provider_batch.batch.columns.end()
                 ? std::nullopt
                 : std::optional<std::size_t>(std::distance(
                       provider_batch.batch.columns.begin(), column));
    };
    const auto vertex_ordinal = column_ordinal("vertex_uuid");
    const auto row_ordinal_column = column_ordinal("row_uuid");
    const auto edge_ordinal = column_ordinal("edge_uuid");
    const auto path_ordinal = column_ordinal("path_uuid");
    const auto labels_ordinal = column_ordinal("vertex_labels");
    const auto vertex_properties_ordinal = column_ordinal("vertex_properties");
    const auto edge_properties_ordinal = column_ordinal("edge_properties");
    const auto direction_ordinal = column_ordinal("direction");
    const auto depth_ordinal = column_ordinal("depth");
    const auto cycle_ordinal = column_ordinal("cycle_policy");
    const auto exact_known_column = [&](const std::optional<std::size_t> ordinal,
                                        const std::string_view type,
                                        const bool nullable) {
      if (!ordinal.has_value()) return true;
      const auto& column = provider_batch.batch.columns[*ordinal];
      return column.descriptor.canonical_type_name == type &&
             column.nullable == nullable;
    };
    if (!exact_known_column(row_ordinal_column, "uuid", false) ||
        !exact_known_column(vertex_ordinal, "uuid", false) ||
        !exact_known_column(edge_ordinal, "uuid", true) ||
        !exact_known_column(path_ordinal, "uuid", false) ||
        !exact_known_column(labels_ordinal, "text", false) ||
        !exact_known_column(vertex_properties_ordinal, "text", false) ||
        !exact_known_column(edge_properties_ordinal, "text", false) ||
        !exact_known_column(direction_ordinal, "text", false) ||
        !exact_known_column(depth_ordinal, "uint64", false) ||
        !exact_known_column(cycle_ordinal, "text", false)) {
      return Refuse(kModelTypedExchangeInvalid,
                    "graph known-column descriptor contract drifted");
    }
    for (std::size_t row_ordinal = 0;
         row_ordinal < provider_batch.batch.rows.size(); ++row_ordinal) {
      const auto& row = provider_batch.batch.rows[row_ordinal];
      const auto& identity =
          provider_batch.ordered_row_identities[row_ordinal];
      if (std::ranges::any_of(row.values, [](const auto& value) {
            return value.state == internal_api::EngineValueState::missing;
          })) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph provider returned an unbound missing value");
      }
      const auto exact_uuid_value = [&](const std::optional<std::size_t> ordinal,
                                        const std::string& expected) {
        if (!ordinal.has_value()) return true;
        const auto& column = provider_batch.batch.columns[*ordinal];
        const auto& value = row.values[*ordinal];
        return column.descriptor.canonical_type_name == "uuid" &&
               value.state == internal_api::EngineValueState::value &&
               CanonicalUuid(value.encoded_value) &&
               value.encoded_value == expected && value.binary_value.empty();
      };
      if (!exact_uuid_value(row_ordinal_column, identity.row_uuid) ||
          !exact_uuid_value(vertex_ordinal, identity.vertex_uuid) ||
          !exact_uuid_value(path_ordinal, identity.path_uuid)) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph row UUID value differs from ordered identity");
      }
      if (edge_ordinal.has_value()) {
        const auto& column = provider_batch.batch.columns[*edge_ordinal];
        const auto& value = row.values[*edge_ordinal];
        const bool exact_edge =
            column.descriptor.canonical_type_name == "uuid" &&
            ((identity.graph_depth == 0 && column.nullable &&
              value.state == internal_api::EngineValueState::sql_null &&
              value.encoded_value.empty() && value.binary_value.empty() &&
              identity.edge_uuid.empty()) ||
             (identity.graph_depth > 0 &&
              value.state == internal_api::EngineValueState::value &&
              CanonicalUuid(value.encoded_value) &&
              value.encoded_value == identity.edge_uuid &&
              value.binary_value.empty()));
        if (!exact_edge) {
          return Refuse(kModelTypedExchangeInvalid,
                        "graph edge value differs from ordered identity");
        }
      }
      if (depth_ordinal.has_value()) {
        const auto& column = provider_batch.batch.columns[*depth_ordinal];
        const auto& value = row.values[*depth_ordinal];
        std::uint64_t parsed = 0;
        const auto converted = std::from_chars(
            value.encoded_value.data(),
            value.encoded_value.data() + value.encoded_value.size(), parsed);
        if (column.descriptor.canonical_type_name != "uint64" ||
            value.state != internal_api::EngineValueState::value ||
            !value.binary_value.empty() ||
            value.encoded_value.empty() || converted.ec != std::errc{} ||
            converted.ptr !=
                value.encoded_value.data() + value.encoded_value.size() ||
            parsed != identity.graph_depth) {
          return Refuse(kModelTypedExchangeInvalid,
                        "graph depth value differs from ordered identity");
        }
      }
      const auto exact_nonnull_text = [&](
          const std::optional<std::size_t> ordinal) {
        return !ordinal.has_value() ||
               (row.values[*ordinal].state ==
                    internal_api::EngineValueState::value &&
                row.values[*ordinal].binary_value.empty());
      };
      if (!exact_nonnull_text(labels_ordinal) ||
          !exact_nonnull_text(vertex_properties_ordinal) ||
          !exact_nonnull_text(edge_properties_ordinal) ||
          !exact_nonnull_text(direction_ordinal) ||
          !exact_nonnull_text(cycle_ordinal) ||
          (direction_ordinal.has_value() &&
           row.values[*direction_ordinal].encoded_value != "outgoing" &&
           row.values[*direction_ordinal].encoded_value != "incoming" &&
           row.values[*direction_ordinal].encoded_value != "both") ||
          (cycle_ordinal.has_value() &&
           row.values[*cycle_ordinal].encoded_value != "visited_set")) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph typed result value/state contract drifted");
      }
    }
  }

  DescriptorBatch normalized = provider_batch.batch;
  std::size_t cell_count = 0;
  std::uint64_t memory_bytes = sizeof(DescriptorBatch);
  const auto account_bytes = [&](const std::uint64_t bytes) {
    if (std::numeric_limits<std::uint64_t>::max() - memory_bytes < bytes) {
      return false;
    }
    memory_bytes += bytes;
    return true;
  };
  for (const auto& identity : provider_batch.ordered_row_identities) {
    if (!account_bytes(sizeof(ModelProviderRowIdentityV1)) ||
        !account_bytes(identity.document_uuid.size()) ||
        !account_bytes(identity.row_uuid.size()) ||
        !account_bytes(identity.vertex_uuid.size()) ||
        !account_bytes(identity.edge_uuid.size()) ||
        !account_bytes(identity.path_uuid.size())) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange identity memory counter overflowed");
    }
  }
  for (const auto& column : normalized.columns) {
    if (!account_bytes(sizeof(ExecutorColumnDescriptor)) ||
        !account_bytes(column.stable_name.size()) ||
        !account_bytes(column.descriptor.descriptor_uuid.canonical.size()) ||
        !account_bytes(column.descriptor.descriptor_kind.size()) ||
        !account_bytes(column.descriptor.canonical_type_name.size()) ||
        !account_bytes(column.descriptor.encoded_descriptor.size())) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange descriptor memory counter overflowed");
    }
  }
  for (auto& row : normalized.rows) {
    if (!account_bytes(sizeof(DescriptorTuple))) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange row memory counter overflowed");
    }
    if (std::numeric_limits<std::size_t>::max() - cell_count <
        row.values.size()) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document exchange cell counter overflowed");
    }
    cell_count += row.values.size();
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      auto& value = row.values[column];
      if (!graph_family && value.state ==
          scratchbird::engine::internal_api::EngineValueState::missing) {
        if (column >= normalized.columns.size() ||
            !normalized.columns[column].nullable) {
          return Refuse(kModelDocumentMissingBindingRefused,
                        "missing document path has no nullable bound output descriptor");
        }
        value.encoded_value.clear();
        value.binary_value.clear();
        value.setState(
            scratchbird::engine::internal_api::EngineValueState::sql_null);
      } else if (graph_family && value.state ==
                     scratchbird::engine::internal_api::EngineValueState::missing) {
        return Refuse(kModelTypedExchangeInvalid,
                      "graph provider returned an unbound missing value");
      }
      if (!account_bytes(sizeof(internal_api::EngineTypedValue)) ||
          !account_bytes(value.descriptor.descriptor_uuid.canonical.size()) ||
          !account_bytes(value.descriptor.descriptor_kind.size()) ||
          !account_bytes(value.descriptor.canonical_type_name.size()) ||
          !account_bytes(value.descriptor.encoded_descriptor.size()) ||
          !account_bytes(value.encoded_value.size()) ||
          !account_bytes(value.binary_value.size())) {
        return Refuse(kModelTypedExchangeInvalid,
                      "document exchange memory counter overflowed");
      }
    }
  }
  if (normalized.rows.size() > input.maximum_rows ||
      cell_count > input.maximum_cells ||
      memory_bytes > input.maximum_memory_bytes) {
    return Refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "document exchange exceeded its bound resource contract");
  }
  const auto validation = ValidateCanonicalDescriptorBatch(
      normalized, input.output_descriptor_ids);
  if (!validation.ok) {
    return Refuse(kModelTypedExchangeInvalid, validation.detail);
  }

  ModelExchangeResultV1 result;
  result.accepted = true;
  result.root_publishable = true;
  result.output.family_id = input.family_id;
  result.output.operation_id = input.operation_id;
  result.output.object_uuid = input.object_uuid;
  result.output.physical_node_id = input.physical_node_id;
  result.output.selected_alternative_uuid = input.selected_alternative_uuid;
  result.output.capability_uuid = input.capability_uuid;
  result.output.provider_uuid = input.provider_uuid;
  result.output.provider_generation = input.provider_generation;
  result.output.result_handle_uuid = input.result_handle_uuid;
  result.output.causal_counter_id = input.causal_counter_id;
  result.output.output_descriptor_ids = input.output_descriptor_ids;
  result.output.ordered_row_identities =
      provider_batch.ordered_row_identities;
  result.output.batch = std::move(normalized);
  result.output.properties = provider_batch.properties;
  result.output.mga_statement_context = input.mga_statement_context;
  result.output.security_receipt_uuid = provider_batch.security_receipt_uuid;
  result.output.exact_exchange_validated = true;
  return result;
}

}  // namespace scratchbird::engine::executor
