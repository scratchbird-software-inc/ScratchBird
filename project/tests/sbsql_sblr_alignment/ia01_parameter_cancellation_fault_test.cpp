// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_parameter_runtime.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

using Bytes = std::vector<std::uint8_t>;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-002332: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::array<std::uint8_t, 16> RawUuid(std::string_view text) {
  std::array<std::uint8_t, 16> result{};
  std::size_t offset = 0;
  int high = -1;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int digit = ch >= '0' && ch <= '9'
                          ? ch - '0'
                          : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    Require(digit >= 0, "fixture UUID contains a non-hexadecimal byte");
    if (high < 0) {
      high = digit;
    } else {
      Require(offset < result.size(), "fixture UUID is oversized");
      result[offset++] = static_cast<std::uint8_t>((high << 4) | digit);
      high = -1;
    }
  }
  Require(offset == result.size() && high < 0,
          "fixture UUID does not contain exactly 16 bytes");
  return result;
}

void Store64(Bytes* bytes, std::size_t offset, std::uint64_t value) {
  Require(bytes != nullptr && offset + 8 <= bytes->size(),
          "fixture integer storage is out of bounds");
  for (unsigned byte = 0; byte != 8; ++byte) {
    (*bytes)[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
  }
}

sblr::SblrOperand TypedOperand(std::uint32_t ordinal, std::string type,
                               std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.ordinal = ordinal;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  operand.value_body.assign(24, 0);
  operand.value_body[0] = 1;
  Store64(&operand.value_body, 16, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  return operand;
}

struct ExactParameterCarrier {
  sblr::SblrOperationEnvelope envelope;
  sblr::SblrParameterValueSetV1 values;
};

ExactParameterCarrier BuildCarrier() {
  constexpr std::string_view kTreeUuid =
      "019f0000-0000-7000-8000-000000000300";
  constexpr std::string_view kCatalogUuid =
      "019f0000-0000-7100-8000-000000000303";
  constexpr std::string_view kSecurityUuid =
      "019f0000-0000-7110-8000-000000000304";
  constexpr std::string_view kStatementUuid =
      "019f0000-0000-7120-8000-000000000303";
  constexpr std::string_view kTransactionUuid =
      "019f0000-0000-7130-8000-000000000313";
  constexpr std::string_view kSnapshotUuid =
      "019f0000-0000-7140-8000-000000000314";
  constexpr std::string_view kMetadataUuid =
      "019f0000-0000-7150-8000-000000000315";
  constexpr std::string_view kSlotUuid =
      "019f0000-0000-7200-8000-000000000301";
  constexpr std::string_view kTypeUuid =
      "019f0000-0000-7300-8000-000000000302";
  constexpr std::string_view kParameterSetUuid =
      "019f0000-0000-7400-8000-000000000303";

  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "ia01.parameter.cancel");
  envelope.opcode_code = 0x1207;
  envelope.result_shape = "query_execute_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019f0000-0000-7500-8000-000000000304";
  envelope.registry_snapshot_uuid = std::string(kCatalogUuid);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_transaction_context = true;

  std::uint32_t ordinal = 1;
  envelope.operands.push_back(
      TypedOperand(ordinal++, "uint16", "relational_wire_version", "2"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_bound_sblr_tree_uuid",
      std::string(kTreeUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_catalog_epoch_uuid",
      std::string(kCatalogUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_security_context_uuid",
      std::string(kSecurityUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_statement_uuid",
      std::string(kStatementUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_owning_transaction_uuid",
      std::string(kTransactionUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_statement_snapshot_uuid",
      std::string(kSnapshotUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uuid", "relational_statement_metadata_snapshot_uuid",
      std::string(kMetadataUuid)));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uint64", "relational_local_transaction_id", "37"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uint64",
      "relational_snapshot_visible_through_local_transaction_id", "35"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "uint32", "relational_root_node_id", "1"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "relational_descriptor_v1", "slot_1",
      std::string(kSlotUuid) + "|" + std::string(kTypeUuid) +
          "|1|-|-|-|-|-"));

  sblr::SblrParameterNodeV1 node;
  node.node_id = 7;
  node.parent_operand_ordinal = 1;
  node.slot_ordinal = 0;
  node.parameter_set_descriptor_uuid = RawUuid(kParameterSetUuid);
  node.parameter_set_generation = 2;
  node.datatype_descriptor_uuid = RawUuid(kTypeUuid);
  node.datatype_descriptor_generation = 4;
  sblr::SblrParameterNodeTableV1 table;
  table.nodes.push_back(node);
  const auto sbpn = sblr::EncodeSblrParameterNodeTableV1(table);
  Require(!sbpn.empty(), "canonical SBPN encoding failed");

  sblr::SblrOperand table_operand;
  table_operand.ordinal = ordinal++;
  table_operand.type = "expression.parameter_node_table.v1";
  table_operand.name = "parameter_nodes";
  table_operand.value_kind = sblr::SblrValueKind::parameter_node_table;
  table_operand.value_body = sbpn;
  envelope.operands.push_back(std::move(table_operand));

  static constexpr std::string_view kTableHashDomain =
      "ScratchBird.SblrParameterNodeTable.V1";
  Bytes table_hash_material(kTableHashDomain.begin(), kTableHashDomain.end());
  table_hash_material.insert(table_hash_material.end(), sbpn.begin(),
                             sbpn.end());
  const auto table_hash =
      scratchbird::core::hash::ComputeSha256Digest(table_hash_material);
  Require(table_hash.ok(), "canonical SBPN hash failed");
  sblr::SblrParameterNodeReferenceV1 reference;
  reference.occurrence_ordinal = 1;
  reference.node_id = node.node_id;
  reference.table_sha256 = table_hash.digest;
  reference.parameter_set_descriptor_uuid =
      node.parameter_set_descriptor_uuid;
  reference.parameter_set_generation = node.parameter_set_generation;
  reference.slot_ordinal = node.slot_ordinal;
  const auto encoded_reference =
      sblr::EncodeSblrParameterNodeReferenceV1(reference);
  Require(!encoded_reference.empty(), "canonical kind-19 encoding failed");
  sblr::SblrOperand reference_operand;
  reference_operand.ordinal = ordinal++;
  reference_operand.type = "relational_expression_v1";
  reference_operand.name = "1";
  reference_operand.value_kind = sblr::SblrValueKind::parameter_node_ref;
  reference_operand.value_body = encoded_reference;
  envelope.operands.push_back(std::move(reference_operand));

  envelope.operands.push_back(TypedOperand(
      ordinal++, "relational_output_v1", "slot_1",
      "1|1|1|1|0|706172616d657465725f76616c7565"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "relational_values_row_v1", "slot_1", "1"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "relational_node_v1", "slot_1", "13|0|-|1|1"));
  envelope.operands.push_back(TypedOperand(
      ordinal++, "relational_node_binding_v1", "slot_1",
      "76616c7565732e706172616d657465722d7461626c652e7631|1|-|-|-"));

  sblr::SblrParameterValueSetV1 values;
  values.parameter_set_descriptor_uuid = node.parameter_set_descriptor_uuid;
  values.descriptor_generation = node.parameter_set_generation;
  values.execution_uuid =
      RawUuid("019f0000-0000-7600-8000-000000000305");
  values.statement_receipt_uuid =
      RawUuid("019f0000-0000-7700-8000-000000000306");
  sblr::SblrParameterValueRecordV1 value;
  value.slot_ordinal = node.slot_ordinal;
  value.slot_uuid = RawUuid(kSlotUuid);
  value.datatype_descriptor_uuid = node.datatype_descriptor_uuid;
  value.datatype_descriptor_generation = node.datatype_descriptor_generation;
  value.direction = sblr::SblrParameterDirectionV1::in;
  value.state = sblr::SblrParameterValueStateV1::value;
  value.canonical_value_bytes = {7, 0, 0, 0, 0, 0, 0, 0};
  values.records.push_back(std::move(value));

  const auto encoded_values = sblr::EncodeSblrParameterValueSetV1(values);
  const auto decoded_values = sblr::DecodeSblrParameterValueSetV1(
      encoded_values.data(), encoded_values.size());
  Require(decoded_values.ok && decoded_values.canonical_bytes == encoded_values,
          "canonical SBPV round trip failed");

  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  if (!validation.ok) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(validation.ok, "canonical parameter query envelope was refused");
  const auto encoded_envelope = sblr::EncodeSblrEnvelope(envelope);
  const auto decoded_envelope = sblr::DecodeSblrEnvelope(encoded_envelope);
  Require(!encoded_envelope.empty() && decoded_envelope.ok &&
              sblr::EncodeSblrEnvelope(decoded_envelope.envelope) ==
                  encoded_envelope,
          "canonical parameter query SBOP round trip failed");
  auto admitted_envelope = decoded_envelope.envelope;
  admitted_envelope.requires_transaction_context = true;
  return {std::move(admitted_envelope), decoded_values.value};
}

api::EngineRequestContext Context(std::atomic<unsigned>* probes) {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000303";
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000000313";
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000000314";
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000000303";
  context.local_transaction_id = 37;
  context.snapshot_visible_through_local_transaction_id = 35;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000000315";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-000000000304";
  context.catalog_generation_id = 303;
  context.security_epoch = 304;
  context.resource_epoch = 305;
  context.query_cancellation_requested = [probes] {
    probes->fetch_add(1, std::memory_order_relaxed);
    return true;
  };
  return context;
}

}  // namespace

int main() {
  auto carrier = BuildCarrier();
  std::atomic<unsigned> probes{0};
  sblr::SblrDispatchRequest request;
  request.context = Context(&probes);
  request.envelope = std::move(carrier.envelope);
  request.parameter_value_set = std::move(carrier.values);

  const auto result = sblr::DispatchSblrOperation(std::move(request));
  Require(result.envelope_validated,
          "canonical parameter query did not cross SBOP validation");
  Require(!result.accepted && !result.dispatched_to_api &&
              !result.logical_graph_populated &&
              !result.logical_properties_populated &&
              !result.optimizer_admitted && !result.optimizer_selected &&
              !result.physical_dag_published &&
              !result.physical_dag_executed &&
              !result.runtime_actuals_attached &&
              !result.canonical_result_published,
          "cancelled parameter query crossed a planning or execution boundary");
  Require(!result.api_result.ok && result.api_result.evidence.empty() &&
              result.api_result.result_shape.rows.empty() &&
              result.canonical_result_bytes.empty() &&
              result.canonical_result_row_count == 0,
          "cancelled parameter query published evidence or result data");
  if (probes.load(std::memory_order_relaxed) != 1) {
    std::cerr << "parameter-cancellation-probes="
              << probes.load(std::memory_order_relaxed) << '\n';
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(probes.load(std::memory_order_relaxed) == 1,
          "parameter cancellation was not consulted exactly at slot entry");
  Require(result.diagnostics.size() == 1 &&
              result.diagnostics.front().code == "PROCESS.CANCELLED" &&
              result.api_result.diagnostics.size() == 1 &&
              result.api_result.diagnostics.front().code ==
                  "PROCESS.CANCELLED" &&
              result.api_result.diagnostics.front().detail ==
                  "parameter evaluation cancelled before slot entry",
          "parameter cancellation diagnostic or precedence drifted");
  return EXIT_SUCCESS;
}
