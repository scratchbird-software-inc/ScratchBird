// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_exchange.hpp"

#include <cctype>
#include <limits>
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
  if (input.abi_version != 1 ||
      input.input_descriptor_id != "SB_MODEL_SOURCE_INPUT_DESCRIPTOR_V1" ||
      input.family_id != "document" ||
      (input.operation_id != "DOCUMENT_FIND" &&
       input.operation_id != "DOCUMENT_PATH" &&
       input.operation_id != "DOCUMENT_UNNEST") ||
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
  std::unordered_set<std::string> document_uuids;
  std::unordered_set<std::string> row_uuids;
  if (provider_batch.ordered_row_identities.size() !=
      provider_batch.batch.rows.size()) {
    return Refuse(kModelTypedExchangeInvalid,
                  "document ordered row identity cardinality is incomplete");
  }
  for (const auto& identity : provider_batch.ordered_row_identities) {
    if (!CanonicalUuid(identity.document_uuid) ||
        !CanonicalUuid(identity.row_uuid) ||
        !document_uuids.insert(identity.document_uuid).second ||
        !row_uuids.insert(identity.row_uuid).second) {
      return Refuse(kModelTypedExchangeInvalid,
                    "document row uniqueness or ordering identity is invalid");
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
      provider_batch.properties.uniqueness_id != "document_uuid" ||
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
                  "document exactness or recheck receipt is incomplete");
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
        !account_bytes(identity.row_uuid.size())) {
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
      if (value.state ==
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
