// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-021-V1: " << detail << '\n';
  }
  return condition;
}

std::string LengthFieldToken(const std::string_view key,
                             const std::string& value) {
  return std::string(key) + "=" + std::to_string(value.size()) + ":" +
         value + "\n";
}

bool ContainsExactStatementIdentityBytes(
    const std::string& bytes,
    const exec::PhysicalMgaStatementContext& context,
    const std::string& catalog_epoch_uuid) {
  const auto number = [](const std::uint64_t value) {
    return std::to_string(value);
  };
  const std::vector<std::string> fields{
      LengthFieldToken("mga.statement_uuid", context.statement_uuid),
      LengthFieldToken("mga.owning_transaction_uuid",
                       context.owning_transaction_uuid),
      LengthFieldToken("mga.statement_snapshot_uuid",
                       context.statement_snapshot_uuid),
      LengthFieldToken("mga.statement_metadata_snapshot_uuid",
                       context.statement_metadata_snapshot_uuid),
      LengthFieldToken("mga.owning_local_transaction_id",
                       number(context.owning_local_transaction_id)),
      LengthFieldToken("mga.visible_committed_high_watermark",
                       number(context.visible_committed_high_watermark)),
      LengthFieldToken("mga.oldest_active_transaction_id",
                       number(context.oldest_active_transaction_id)),
      LengthFieldToken("mga.oldest_interesting_transaction_id",
                       number(context.oldest_interesting_transaction_id)),
      LengthFieldToken("mga.oldest_snapshot_transaction_id",
                       number(context.oldest_snapshot_transaction_id)),
      LengthFieldToken("mga.retention_horizon_transaction_id",
                       number(context.retention_horizon_transaction_id)),
      LengthFieldToken("mga.active_excluded_local_transaction_id_count", "2"),
      LengthFieldToken("mga.active_excluded_local_transaction_id", "7"),
      LengthFieldToken("mga.active_excluded_local_transaction_id", "9"),
      LengthFieldToken("mga.in_doubt_excluded_local_transaction_id_count", "1"),
      LengthFieldToken("mga.in_doubt_excluded_local_transaction_id", "8"),
      LengthFieldToken("mga.snapshot_kind", context.snapshot_kind),
      LengthFieldToken(
          "mga.publication_inventory_next_local_transaction_id",
          number(context.publication_inventory_next_local_transaction_id)),
      LengthFieldToken("mga.inventory_authoritative", "true"),
      LengthFieldToken("mga.complete", "true"),
      LengthFieldToken("mga.current", "true"),
      LengthFieldToken("catalog_epoch_uuid", catalog_epoch_uuid),
  };
  std::size_t position = 0;
  for (const auto& field : fields) {
    position = bytes.find(field, position);
    if (position == std::string::npos) return false;
    position += field.size();
  }
  return true;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& canonical_type,
                                 const std::string& nullability,
                                 const std::string& extra = {}) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability + extra;
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue SqlNull(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.setState(api::EngineValueState::sql_null);
  return value;
}

exec::PhysicalMgaStatementContext StatementContext(
    const bool zero_high_water = false,
    const bool maximum_inventory_local_transaction_number = false) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = "019f0000-0000-7100-8000-000000002101";
  context.owning_transaction_uuid =
      "019f0000-0000-7100-8000-000000002104";
  context.statement_snapshot_uuid =
      "019f0000-0000-7100-8000-000000002105";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7100-8000-000000002106";
  context.owning_local_transaction_id = 7;
  context.visible_committed_high_watermark = zero_high_water ? 0 : 6;
  context.oldest_active_transaction_id = 7;
  context.oldest_interesting_transaction_id = 3;
  context.oldest_snapshot_transaction_id = 3;
  context.retention_horizon_transaction_id = 3;
  context.active_excluded_local_transaction_ids = {7, 9};
  context.in_doubt_excluded_local_transaction_ids = {8};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id =
      maximum_inventory_local_transaction_number
          ? std::numeric_limits<std::uint64_t>::max()
          : 10;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

exec::TypedPhysicalNodeDag SelectedDag(
    const exec::PhysicalMgaStatementContext& context) {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = "019f0000-0000-7100-8000-000000002130";
  dag.root_physical_node_id = 1;
  dag.local_transaction_id = context.owning_local_transaction_id;
  dag.statement_snapshot_id = context.visible_committed_high_watermark;
  dag.mga_statement_context = context;
  dag.bound_sblr_tree_uuid = "019f0000-0000-7100-8000-000000002131";
  dag.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000002132";
  dag.security_context_uuid = "019f0000-0000-7100-8000-000000002133";
  dag.capability_snapshot_uuid =
      "019f0000-0000-7100-8000-000000002134";
  dag.resource_snapshot_uuid =
      "019f0000-0000-7100-8000-000000002135";
  dag.statistics_snapshot_uuid =
      "019f0000-0000-7100-8000-000000002136";
  dag.route_snapshot_uuid = "019f0000-0000-7100-8000-000000002137";
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1024;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  const std::vector<std::string> evidence{
      dag.bound_sblr_tree_uuid,
      dag.catalog_epoch_uuid,
      dag.security_context_uuid,
      context.statement_snapshot_uuid,
      dag.capability_snapshot_uuid,
      dag.resource_snapshot_uuid,
      dag.statistics_snapshot_uuid,
      dag.route_snapshot_uuid,
  };
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    dag.admission_evidence.push_back(
        {static_cast<exec::PhysicalAdmissionStage>(index + 1),
         evidence[index]});
  }
  exec::PhysicalNodeRecord node;
  node.physical_node_id = 1;
  node.relational_node_id = 1;
  node.node_kind = exec::PhysicalNodeKind::kValues;
  node.implementation_id = "values.materialize.v1";
  node.output_descriptor_ids = {2101, 2102, 2103};
  node.causal_counter_id = 1;
  node.selected_alternative_uuid =
      "019f0000-0000-7100-8000-000000002140";
  node.executor_capability_uuid =
      "019f0000-0000-7100-8000-000000002141";
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid = "019f0000-0000-7100-8000-000000002142";
  node.memory_bytes_required = 1;
  node.engine_capability_validated = true;
  node.mga_statement_context = context;
  dag.nodes.push_back(std::move(node));
  return dag;
}

struct CurrentAuthorityState {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  exec::PhysicalMgaStatementContext statement_context;
};

exec::CanonicalResultPublicationRequest RowsRequest(
    std::shared_ptr<CurrentAuthorityState> state = {},
    const bool zero_high_water = false,
    const bool maximum_inventory_local_transaction_number = false) {
  const auto id = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002101", "int64", "non_null");
  const auto hidden = Descriptor(
      "019f0000-0000-7200-8000-000000002102",
      "019f0000-0000-7300-8000-000000002102", "text", "non_null");
  const auto label = Descriptor(
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7300-8000-000000002103", "text", "nullable",
      ";collation_uuid=019f0000-0000-7400-8000-000000002103");

  exec::CanonicalResultPublicationRequest request;
  const auto statement_context = StatementContext(
      zero_high_water, maximum_inventory_local_transaction_number);
  request.statement_uuid = statement_context.statement_uuid;
  request.selected_physical_dag = SelectedDag(statement_context);
  request.selected_catalog_epoch_uuid =
      request.selected_physical_dag.catalog_epoch_uuid;
  request.mga_authority.statement_context = statement_context;
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  if (!state) state = std::make_shared<CurrentAuthorityState>();
  state->statement_context = statement_context;
  request.mga_authority.resolve_current = [state] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.diagnostic = state->diagnostic;
    resolution.statement_context = state->statement_context;
    return resolution;
  };
  request.execution_attempt_uuid =
      "019f0000-0000-7110-8000-000000002101";
  request.transaction_effect_evidence_uuid =
      "019f0000-0000-7120-8000-000000002101";
  request.result_kind = exec::CanonicalResultKind::kRows;
  request.physical_output_batch = exec::MakeDescriptorBatch(
      {{"dup", id, false, 2101},
       {"internal_sort_key", hidden, false, 2102},
       {"dup", label, true, 2103}},
      {{{Value(id, "1"), Value(hidden, "z"), Value(label, "alpha")}},
       {{Value(id, "2"), Value(hidden, "y"), SqlNull(label)}}});
  request.column_bindings = {
      {0,
       true,
       exec::CanonicalResultColumnDescriptor{
           0, "dup", id.descriptor_uuid.canonical,
           "019f0000-0000-7300-8000-000000002101",
           exec::CanonicalResultNullability::kNonNull, std::nullopt,
           std::nullopt}},
      {1, false, std::nullopt},
      {2,
       true,
       exec::CanonicalResultColumnDescriptor{
           1, "dup", label.descriptor_uuid.canonical,
           "019f0000-0000-7300-8000-000000002103",
           exec::CanonicalResultNullability::kNullable,
           "019f0000-0000-7400-8000-000000002103", std::nullopt}},
  };
  return request;
}

exec::CanonicalResultPublicationRequest ClosureRowsRequest(
    const bool zero_high_water = false,
    const bool maximum_inventory_local_transaction_number = false) {
  auto request = RowsRequest(
      {}, zero_high_water, maximum_inventory_local_transaction_number);
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto immutable_current_context =
      request.mga_authority.statement_context;
  request.mga_authority.resolve_current = [immutable_current_context] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = immutable_current_context;
    return resolution;
  };
  return request;
}

bool ValidateRowsEmptyCursorAndParity() {
  bool passed = true;
  const auto direct = exec::PublishCanonicalResultEnvelope(RowsRequest());
  passed &= Require(
      direct.diagnostic.ok && direct.published &&
          direct.envelope.abi_version == 1 &&
          exec::PhysicalMgaStatementContextEqual(
              direct.envelope.mga_statement_context,
              StatementContext()) &&
          direct.envelope.catalog_epoch_uuid ==
              "019f0000-0000-7100-8000-000000002132" &&
          direct.envelope.result_kind == exec::CanonicalResultKind::kRows &&
          direct.envelope.row_count == 2 &&
          direct.envelope.column_descriptors.size() == 2 &&
          direct.envelope.column_descriptors[0].ordinal == 0 &&
          direct.envelope.column_descriptors[1].ordinal == 1 &&
          direct.envelope.column_descriptors[0].name_utf8 == "dup" &&
          direct.envelope.column_descriptors[1].name_utf8 == "dup",
      "row result metadata, row count, or duplicate names differ");
  passed &= Require(
      direct.row_stream.columns.size() == 2 &&
          direct.row_stream.rows.size() == 2 &&
          direct.row_stream.rows[0].values[0].encoded_value == "1" &&
          direct.row_stream.rows[0].values[1].encoded_value == "alpha" &&
          direct.row_stream.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          direct.row_stream.rows[1].values[1].encoded_value.empty() &&
          direct.delivery_records.size() == 3 &&
          direct.delivery_records.front().kind ==
              exec::CanonicalResultDeliveryKind::kMetadata &&
          direct.delivery_records[1].kind ==
              exec::CanonicalResultDeliveryKind::kRow,
      "hidden projection, typed NULL, or metadata-before-row delivery differs");
  passed &= Require(
      direct.canonical_envelope_bytes.find("internal_sort_key") ==
              std::string::npos &&
          direct.canonical_envelope_bytes.find(
              "QOW-RESULT-DIAGNOSTIC-ABI-V1") != std::string::npos &&
          ContainsExactStatementIdentityBytes(
              direct.canonical_envelope_bytes, StatementContext(),
              "019f0000-0000-7100-8000-000000002132"),
      "canonical envelope leaked a hidden column or omitted statement identity");

  auto prepared_request = RowsRequest();
  prepared_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kPrepared;
  const auto prepared =
      exec::PublishCanonicalResultEnvelope(prepared_request);
  passed &= Require(
      prepared.diagnostic.ok && prepared.published &&
          prepared.canonical_envelope_bytes == direct.canonical_envelope_bytes &&
          prepared.row_stream.rows.size() == direct.row_stream.rows.size(),
      "prepared execution changed canonical result bytes or typed rows");

  auto empty_request = RowsRequest();
  empty_request.result_kind = exec::CanonicalResultKind::kEmpty;
  empty_request.physical_output_batch.rows.clear();
  const auto empty = exec::PublishCanonicalResultEnvelope(empty_request);
  passed &= Require(
      empty.diagnostic.ok && empty.published && empty.envelope.row_count == 0 &&
          empty.envelope.column_descriptors.size() == 2 &&
          empty.row_stream.rows.empty() &&
          empty.delivery_records.size() == 1 &&
          empty.delivery_records.front().kind ==
              exec::CanonicalResultDeliveryKind::kMetadata,
      "empty result lost metadata or published a row");

  auto cursor_request = RowsRequest();
  cursor_request.result_kind = exec::CanonicalResultKind::kCursor;
  cursor_request.cursor_state = exec::CanonicalResultCursorState::kOpen;
  const auto cursor = exec::PublishCanonicalResultEnvelope(cursor_request);
  passed &= Require(
      cursor.diagnostic.ok && cursor.published &&
          !cursor.envelope.row_count.has_value() &&
          cursor.envelope.cursor_state ==
              exec::CanonicalResultCursorState::kOpen &&
          cursor.row_stream.rows.size() == 2,
      "cursor result lost state, metadata, or its typed row chunk");

  auto command_request = RowsRequest();
  command_request.result_kind = exec::CanonicalResultKind::kCommand;
  command_request.physical_output_batch = {};
  command_request.column_bindings.clear();
  command_request.command_tag = "UPDATE 2";
  const auto command = exec::PublishCanonicalResultEnvelope(command_request);
  passed &= Require(
      command.diagnostic.ok && command.published &&
          command.envelope.command_tag == "UPDATE 2" &&
          !command.envelope.row_count.has_value() &&
          command.envelope.column_descriptors.empty(),
      "command result did not use the shared envelope fields");
  return passed;
}

bool ValidateDiagnosticAndCancellation() {
  auto request = RowsRequest();
  const auto argument_descriptor =
      request.physical_output_batch.columns.front().descriptor;
  request.result_kind = exec::CanonicalResultKind::kEmpty;
  request.physical_output_batch = {};
  request.column_bindings.clear();
  request.diagnostics = {{
      "QOW-DIAGNOSTIC-INSTANCE-2101",
      "SB_EXECUTION_CANCELLED",
      exec::CanonicalResultDiagnosticSeverity::kError,
      "57014",
      "query.execution.cancelled",
      {Value(argument_descriptor, "17")},
      exec::CanonicalResultDiagnosticPhase::kExecute,
      "physical.nodes[0]",
      "cancellation_probe",
      2101,
      exec::CanonicalResultTransactionEffect::
          kStatementFailedTransactionUsable,
      exec::CanonicalResultRetryability::kNotRetryable,
  }};
  const auto result = exec::PublishCanonicalResultEnvelope(request);
  bool passed = true;
  passed &= Require(
      result.diagnostic.ok && result.published &&
          result.envelope.diagnostics.size() == 1 &&
          result.envelope.diagnostics.front().transaction_effect ==
              exec::CanonicalResultTransactionEffect::
                  kStatementFailedTransactionUsable &&
          result.envelope.row_count == 0 && result.row_stream.rows.empty() &&
          result.delivery_records.size() == 2 &&
          result.delivery_records[0].kind ==
              exec::CanonicalResultDeliveryKind::kMetadata &&
          result.delivery_records[1].kind ==
              exec::CanonicalResultDeliveryKind::kDiagnostics,
      "cancellation diagnostic or engine transaction effect was not preserved");
  passed &= Require(
      result.canonical_envelope_bytes.find("query.execution.cancelled") !=
              std::string::npos &&
          result.canonical_envelope_bytes.find("statement_failed_transaction_usable") !=
              std::string::npos,
      "diagnostic fields were omitted from canonical bytes");
  return passed;
}

bool RefusedAtomically(const exec::CanonicalResultPublicationRequest& request) {
  const auto result = exec::PublishCanonicalResultEnvelope(request);
  return !result.diagnostic.ok && !result.published &&
         !result.diagnostic.diagnostic_code.empty() &&
         result.envelope.statement_uuid.empty() &&
         result.envelope.mga_statement_context.statement_uuid.empty() &&
         result.envelope.catalog_epoch_uuid.empty() &&
         result.envelope.execution_attempt_uuid.empty() &&
         result.envelope.column_descriptors.empty() &&
         result.envelope.row_stream_format_id.empty() &&
         !result.envelope.row_count.has_value() &&
         !result.envelope.command_tag.has_value() &&
         !result.envelope.cursor_state.has_value() &&
         result.envelope.diagnostics.empty() &&
         result.row_stream.columns.empty() && result.row_stream.rows.empty() &&
         result.delivery_records.empty() && result.canonical_envelope_bytes.empty();
}

bool ValidateAtomicRefusals() {
  bool passed = true;
  auto request = RowsRequest();
  request.abi_version = 2;
  passed &= Require(RefusedAtomically(request),
                    "unknown ABI version was published");

  request = RowsRequest();
  request.transaction_effect_evidence_uuid.clear();
  passed &= Require(RefusedAtomically(request),
                    "missing engine transaction-effect evidence was accepted");

  request = RowsRequest();
  request.column_bindings[1].published_descriptor =
      exec::CanonicalResultColumnDescriptor{};
  passed &= Require(RefusedAtomically(request),
                    "hidden column published descriptor metadata");

  request = RowsRequest();
  request.column_bindings[2].published_descriptor->type_uuid =
      "019f0000-0000-7300-8000-000000002199";
  passed &= Require(RefusedAtomically(request),
                    "result descriptor drifted from physical authority");

  request = RowsRequest();
  request.physical_output_batch.rows[1].values[2].setState(
      api::EngineValueState::missing);
  passed &= Require(RefusedAtomically(request),
                    "malformed later row published partial metadata or rows");

  request = RowsRequest();
  request.maximum_row_count = 1;
  passed &= Require(RefusedAtomically(request),
                    "result publication ignored the row bound");

  request = RowsRequest();
  request.result_kind = exec::CanonicalResultKind::kCursor;
  passed &= Require(RefusedAtomically(request),
                    "cursor without state was published");

  request = RowsRequest();
  request.diagnostics = {{
      "QOW-DIAGNOSTIC-INSTANCE-2199",
      "SB_BAD_DIAGNOSTIC",
      static_cast<exec::CanonicalResultDiagnosticSeverity>(255),
      std::nullopt,
      "bad.diagnostic",
      {},
      exec::CanonicalResultDiagnosticPhase::kExecute,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      exec::CanonicalResultTransactionEffect::kUnchanged,
      exec::CanonicalResultRetryability::kNotRetryable,
  }};
  passed &= Require(RefusedAtomically(request),
                    "unknown diagnostic enum was published");
  return passed;
}

bool ValidateStatementAuthorityPublicationBoundary() {
  bool passed = true;

  const auto zero = exec::PublishCanonicalResultEnvelope(
      RowsRequest({}, true));
  passed &= Require(
      zero.diagnostic.ok && zero.published &&
          zero.envelope.mga_statement_context
                  .visible_committed_high_watermark == 0 &&
          zero.canonical_envelope_bytes.find(
              "mga.visible_committed_high_watermark=1:0") !=
              std::string::npos,
      "valid zero committed high-water statement was refused or not encoded");

  auto request = RowsRequest();
  request.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing;
  passed &= Require(RefusedAtomically(request),
                    "missing engine-inventory origin published output");

  request = RowsRequest();
  request.mga_authority.resolve_current = {};
  passed &= Require(RefusedAtomically(request),
                    "nil current-authority resolver published output");

  request = RowsRequest();
  request.mga_authority.statement_context.current = false;
  passed &= Require(RefusedAtomically(request),
                    "malformed authority context published output");

  auto state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  state->statement_context.current = false;
  passed &= Require(RefusedAtomically(request),
                    "non-current resolved statement published output");

  request = RowsRequest();
  request.mga_authority.statement_context.owning_transaction_uuid =
      "019f0000-0000-7100-8000-000000002199";
  passed &= Require(RefusedAtomically(request),
                    "wrong owning transaction published output");

  request = RowsRequest();
  request.statement_uuid = "019f0000-0000-7100-8000-000000002198";
  passed &= Require(RefusedAtomically(request),
                    "cross-statement scalar identity published output");

  state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  request.mga_authority.statement_context
      .active_excluded_local_transaction_ids = {7};
  request.selected_physical_dag.mga_statement_context =
      request.mga_authority.statement_context;
  request.selected_physical_dag.nodes.front().mga_statement_context =
      request.mga_authority.statement_context;
  passed &= Require(RefusedAtomically(request),
                    "narrowed active exclusion set published output");

  state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  request.mga_authority.statement_context
      .active_excluded_local_transaction_ids = {7, 8};
  request.mga_authority.statement_context
      .in_doubt_excluded_local_transaction_ids = {9};
  request.selected_physical_dag.mga_statement_context =
      request.mga_authority.statement_context;
  request.selected_physical_dag.nodes.front().mga_statement_context =
      request.mga_authority.statement_context;
  passed &= Require(RefusedAtomically(request),
                    "swapped exclusion-set membership published output");

  request = RowsRequest();
  request.mga_authority.statement_context
      .active_excluded_local_transaction_ids = {9, 7};
  request.selected_physical_dag.mga_statement_context =
      request.mga_authority.statement_context;
  request.selected_physical_dag.nodes.front().mga_statement_context =
      request.mga_authority.statement_context;
  passed &= Require(RefusedAtomically(request),
                    "reordered exclusions published output");

  request = RowsRequest();
  request.selected_catalog_epoch_uuid =
      "019f0000-0000-7100-8000-000000002197";
  passed &= Require(RefusedAtomically(request),
                    "conflicting selected catalog epoch published output");

  state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  state->diagnostic.ok = false;
  state->diagnostic.diagnostic_code = "SB_TEST_INVENTORY_RESOLUTION_REFUSED";
  state->diagnostic.detail = "durable inventory unavailable";
  passed &= Require(RefusedAtomically(request),
                    "resolver refusal published output");

  return passed;
}

bool ValidateCompileBoundedClosureAuthorityPublicationBoundary() {
  bool passed = true;

  const auto exact =
      exec::PublishCanonicalResultEnvelope(ClosureRowsRequest());
  passed &= Require(
      exact.diagnostic.ok && exact.published &&
          exec::PhysicalMgaStatementContextEqual(
              exact.envelope.mga_statement_context, StatementContext()) &&
          ContainsExactStatementIdentityBytes(
              exact.canonical_envelope_bytes, StatementContext(),
              "019f0000-0000-7100-8000-000000002132"),
      "exact immutable closure authority did not publish its full context");

  const auto zero =
      exec::PublishCanonicalResultEnvelope(ClosureRowsRequest(true));
  passed &= Require(
      zero.diagnostic.ok && zero.published &&
          zero.envelope.mga_statement_context
                  .visible_committed_high_watermark == 0 &&
          zero.canonical_envelope_bytes.find(
              "mga.visible_committed_high_watermark=1:0") !=
              std::string::npos,
      "exact closure authority narrowed or refused a zero high-watermark");

  const auto maximum = exec::PublishCanonicalResultEnvelope(
      ClosureRowsRequest(false, true));
  const auto maximum_local_transaction_number =
      std::numeric_limits<std::uint64_t>::max();
  passed &= Require(
      maximum.diagnostic.ok && maximum.published &&
          maximum.envelope.mga_statement_context
                  .publication_inventory_next_local_transaction_id ==
              maximum_local_transaction_number &&
          maximum.canonical_envelope_bytes.find(
              LengthFieldToken(
                  "mga.publication_inventory_next_local_transaction_id",
                  std::to_string(maximum_local_transaction_number))) !=
              std::string::npos,
      "exact closure authority narrowed or refused UINT64_MAX inventory identity");

  auto request = ClosureRowsRequest();
  request.mga_authority.resolve_current = {};
  passed &= Require(RefusedAtomically(request),
                    "closure origin without a current resolver published output");

  request = ClosureRowsRequest();
  request.mga_authority.statement_context.statement_metadata_snapshot_uuid =
      "00000000-0000-0000-0000-000000000000";
  passed &= Require(RefusedAtomically(request),
                    "closure origin with a nil context identity published output");

  auto state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  state->statement_context.current = false;
  passed &= Require(RefusedAtomically(request),
                    "closure authority published after current-state change");

  state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  state->statement_context.owning_transaction_uuid =
      "019f0000-0000-7100-8000-000000002199";
  passed &= Require(RefusedAtomically(request),
                    "closure resolver identity mismatch published output");

  state = std::make_shared<CurrentAuthorityState>();
  request = RowsRequest(state);
  request.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  state->diagnostic.ok = false;
  state->diagnostic.diagnostic_code = "SB_TEST_CLOSURE_RESOLUTION_REFUSED";
  state->diagnostic.detail = "closure resolver refused current authority";
  passed &= Require(RefusedAtomically(request),
                    "closure resolver refusal published output");

  return passed;
}

}  // namespace

// QOW-TEST-QRY-021-V1
// QOW-TEST-IAS-010-V1
int main() {
  bool passed = true;
  passed &= ValidateRowsEmptyCursorAndParity();
  passed &= ValidateDiagnosticAndCancellation();
  passed &= ValidateAtomicRefusals();
  passed &= ValidateStatementAuthorityPublicationBoundary();
  passed &= ValidateCompileBoundedClosureAuthorityPublicationBoundary();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
