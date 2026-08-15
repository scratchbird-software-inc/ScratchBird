// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::executor {

inline constexpr const char* kModelTypedExchangeInvalid =
    "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
inline constexpr const char* kModelDocumentMissingBindingRefused =
    "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1";
inline constexpr const char* kModelMgaContextMismatch =
    "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";

struct ModelPropertyDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string property_descriptor_id{"SB_MODEL_PROPERTY_DESCRIPTOR_V1"};
  std::string property_uuid;
  std::string ordering_id{"fixture_order"};
  std::string partitioning_id{"single_local_partition"};
  std::string uniqueness_id{"document_uuid"};
  bool exact{true};
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
};

struct ModelProviderRowIdentityV1 {
  std::string document_uuid;
  std::string row_uuid;
  std::string vertex_uuid;
  std::string edge_uuid;
  std::string path_uuid;
  std::uint64_t graph_depth{0};
  std::string key;
  std::string series_uuid;
  std::string metric_uuid;
  std::string tags;
  std::int64_t point_timestamp_ns{0};
  std::int64_t bucket_start_ns{0};
  // Exact provider payload receipt for time-series exchange. The kind
  // distinguishes raw REAL64, each downsample REAL64 aggregate, and COUNT
  // INT64 so a valid-but-different typed cell cannot be substituted after
  // the engine provider produced its result.
  std::string time_series_payload_kind;
  std::string time_series_raw_value;
  std::string time_series_sample_count;
  std::string time_series_aggregate_value;
  // RCP-077 exact public vector result identity. These canonical REAL64
  // strings are rechecked against the typed batch before publication.
  std::string vector_distance;
  std::string vector_score;
  // RCP-078 exact public search result identity. The relation UUID lives in
  // document_uuid; analyzer identity, score, and gap-free rank are carried
  // independently so the typed exchange can reject cell substitution.
  std::string search_analyzer_uuid;
  std::uint64_t search_analyzer_generation{0};
  std::string search_score;
  std::uint64_t search_rank{0};
};

struct ModelSourceInputDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string input_descriptor_id{"SB_MODEL_SOURCE_INPUT_DESCRIPTOR_V1"};
  std::string family_id;
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string object_uuid;
  // KEY_VALUE_MULTI_GET binds the first-distinct request sequence here so
  // the typed exchange can prove that the provider result is its ordered
  // subsequence. This remains empty for every other operation and family.
  std::vector<std::string> key_value_request_order;
  std::size_t maximum_key_value_request_count{0};
  std::uint64_t maximum_key_value_request_bytes{0};
  // RCP-079 spatial bindings are engine-resolved before provider access.
  // These remain empty for every non-spatial family.
  std::string spatial_geometry_descriptor_uuid;
  std::string spatial_geometry_type_uuid;
  std::string spatial_crs_uuid;
  std::uint64_t spatial_crs_generation{0};
  std::uint64_t physical_node_id{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  PhysicalMgaStatementContext mga_statement_context;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string policy_snapshot_uuid;
  std::string resource_contract_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t descriptor_generation{0};
  std::uint64_t security_generation{0};
  std::uint64_t policy_generation{0};
  std::uint64_t resource_generation{0};
  std::size_t maximum_rows{0};
  std::size_t maximum_cells{0};
  std::uint64_t maximum_memory_bytes{0};
  // RCP-080 common-context authority. A non-timestamp family may retain the
  // immutable timestamp required by a timestamp-carrying sibling only inside
  // an admitted 3--9-leg composition. Single-family validation remains
  // unchanged and cannot mint or remove the timestamp.
  std::string multimodel_composition_receipt_uuid;
  std::uint16_t multimodel_lexical_source_ordinal{0};
  std::uint16_t multimodel_composition_arity{0};
  bool multimodel_common_statement_context{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool exact_fallback_selected{false};
};

struct ModelProviderBatchV1 {
  std::uint16_t abi_version{1};
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  bool exact_fallback_selected{false};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<ModelProviderRowIdentityV1> ordered_row_identities;
  DescriptorBatch batch;
  ModelPropertyDescriptorV1 properties;
  PhysicalMgaStatementContext mga_statement_context;
  std::string security_receipt_uuid;
  std::string multimodel_composition_receipt_uuid;
  std::uint16_t multimodel_lexical_source_ordinal{0};
  std::uint16_t multimodel_composition_arity{0};
  bool multimodel_common_statement_context{false};
  bool residual_recheck_complete{false};
  bool base_row_mga_recheck_complete{false};
  bool security_recheck_complete{false};
  bool provider_visibility_authority_claimed{false};
  bool provider_finality_authority_claimed{false};
};

struct ModelSourceOutputDescriptorV1 {
  std::uint16_t abi_version{1};
  std::string output_descriptor_id{"SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1"};
  std::string family_id;
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string object_uuid;
  std::string spatial_geometry_descriptor_uuid;
  std::string spatial_geometry_type_uuid;
  std::string spatial_crs_uuid;
  std::uint64_t spatial_crs_generation{0};
  std::uint64_t physical_node_id{0};
  std::string selected_alternative_uuid;
  std::string capability_uuid;
  std::string provider_uuid;
  std::uint64_t provider_generation{0};
  std::string result_handle_uuid;
  std::uint64_t causal_counter_id{0};
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<ModelProviderRowIdentityV1> ordered_row_identities;
  DescriptorBatch batch;
  ModelPropertyDescriptorV1 properties;
  PhysicalMgaStatementContext mga_statement_context;
  std::string security_receipt_uuid;
  std::string multimodel_composition_receipt_uuid;
  std::uint16_t multimodel_lexical_source_ordinal{0};
  std::uint16_t multimodel_composition_arity{0};
  bool multimodel_common_statement_context{false};
  bool exact_exchange_validated{false};
  bool exact_fallback_selected{false};
};

struct ModelExchangeResultV1 {
  bool accepted{false};
  bool root_publishable{false};
  ModelSourceOutputDescriptorV1 output;
  std::string diagnostic_id;
  std::string detail;
};

struct ModelInputValidationResultV1 {
  bool accepted{false};
  std::string diagnostic_id;
  std::string detail;
};

ModelInputValidationResultV1 ValidateModelFamilySourceInputV1(
    const ModelSourceInputDescriptorV1& input);

ModelExchangeResultV1 PublishModelFamilyExchangeV1(
    const ModelSourceInputDescriptorV1& input,
    const ModelProviderBatchV1& provider_batch,
    const std::function<bool()>& cancellation_requested = {});

bool ValidateCanonicalTimeSeriesTagsV1(
    std::string_view tags,
    const std::function<bool()>& cancellation_requested = {},
    bool* cancellation_observed = nullptr);

bool ParseCanonicalTimeSeriesTimestampNsV1(std::string_view timestamp,
                                           std::int64_t* timestamp_ns);

// RCP-076 canonical typed-relational ASOF join. Both inputs are already
// engine-owned descriptor batches; the temporal key carrier only binds the
// exact metric/tag/timestamp semantics used by the selected two-input JOIN.
struct CanonicalTimeSeriesAsofKeyV1 {
  std::string metric_uuid;
  std::string canonical_tags;
  std::int64_t timestamp_ns{0};
};

struct CanonicalTimeSeriesAsofInputBindingV1 {
  std::uint32_t metric_expression_id{0};
  std::uint32_t tags_expression_id{0};
  std::uint32_t timestamp_expression_id{0};
  std::uint32_t row_uuid_expression_id{0};
  std::uint32_t metric_descriptor_id{0};
  std::uint32_t tags_descriptor_id{0};
  std::uint32_t timestamp_descriptor_id{0};
  std::uint32_t row_uuid_descriptor_id{0};
  std::size_t metric_column_ordinal{0};
  std::size_t tags_column_ordinal{0};
  std::size_t timestamp_column_ordinal{0};
  std::size_t row_uuid_column_ordinal{0};
  bool raw_time_series{false};
  bool downsample_time_series{false};
};

struct CanonicalTimeSeriesAsofJoinRequestV1 {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id{0};
  DescriptorBatch left_batch;
  DescriptorBatch right_batch;
  std::vector<CanonicalTimeSeriesAsofKeyV1> left_keys;
  std::vector<CanonicalTimeSeriesAsofKeyV1> right_keys;
  std::vector<std::string> right_tie_break_row_uuids;
  CanonicalTimeSeriesAsofInputBindingV1 left_binding;
  CanonicalTimeSeriesAsofInputBindingV1 right_binding;
  std::int64_t tolerance_ns{0};
  bool left_outer{true};
  bool right_is_time_series_raw{false};
  std::size_t maximum_output_rows{0};
  std::uint64_t maximum_comparisons{0};
  std::uint64_t maximum_memory_bytes{0};
  CanonicalExecutionMgaAuthority mga_authority;
  std::function<bool()> cancellation_requested;
};

struct CanonicalTimeSeriesAsofJoinResultV1 {
  DescriptorRuntimeDiagnostic diagnostic;
  DescriptorBatch output_batch;
  std::vector<std::int64_t> matched_right_ordinals;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id{0};
  std::uint64_t causal_counter_id{0};
  PhysicalMgaStatementContext mga_statement_context;
};

std::string CanonicalTimeSeriesAsofTransformationReceiptV1(
    const CanonicalTimeSeriesAsofJoinRequestV1& request);

CanonicalTimeSeriesAsofJoinResultV1 ExecuteCanonicalTimeSeriesAsofJoinV1(
    const CanonicalTimeSeriesAsofJoinRequestV1& request);

}  // namespace scratchbird::engine::executor
