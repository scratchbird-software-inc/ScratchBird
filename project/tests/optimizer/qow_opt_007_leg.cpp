// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/engine/executor/model_family_exchange.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace executor = scratchbird::engine::executor;

namespace {

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "019f0000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

executor::PhysicalMgaStatementContext Mga(const bool timestamp) {
  executor::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(1);
  context.statement_timestamp = timestamp ? "2026-08-12T20:00:00Z" : "";
  context.owning_transaction_uuid = Uuid(2);
  context.statement_snapshot_uuid = Uuid(3);
  context.statement_metadata_snapshot_uuid = Uuid(4);
  context.owning_local_transaction_id = 40;
  context.visible_committed_high_watermark = 39;
  context.oldest_active_transaction_id = 30;
  context.oldest_interesting_transaction_id = 29;
  context.oldest_snapshot_transaction_id = 29;
  context.retention_horizon_transaction_id = 29;
  context.active_excluded_local_transaction_ids = {40};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 41;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

executor::ModelSourceInputDescriptorV1 Input(const std::string& family,
                                              const bool timestamp) {
  executor::ModelSourceInputDescriptorV1 input;
  input.family_id = family;
  input.operation_id =
      family == "relational" ? "RELATIONAL_HEAP_SCAN"
      : family == "document" ? "DOCUMENT_FIND"
                             : "GRAPH_MATCH";
  input.object_uuid = Uuid(10);
  input.physical_node_id = 1;
  input.selected_alternative_uuid = Uuid(11);
  input.capability_uuid = Uuid(12);
  input.provider_uuid = Uuid(13);
  input.provider_generation = 1;
  input.result_handle_uuid = Uuid(14);
  input.causal_counter_id = 1;
  input.output_descriptor_ids = {1};
  input.mga_statement_context = Mga(timestamp);
  input.catalog_epoch_uuid = Uuid(15);
  input.security_context_uuid = Uuid(16);
  input.policy_snapshot_uuid = Uuid(17);
  input.resource_contract_uuid = Uuid(18);
  input.catalog_generation = 1;
  input.descriptor_generation = 1;
  input.security_generation = 1;
  input.policy_generation = 1;
  input.resource_generation = 1;
  input.maximum_rows = 4;
  input.maximum_cells = 4;
  input.maximum_memory_bytes = 4096;
  return input;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-OPT-007-LEG: " << detail << '\n';
  return condition;
}

bool Refused(const executor::ModelSourceInputDescriptorV1& input,
             const std::string_view diagnostic =
                 "SB_MODEL_MGA_CONTEXT_MISMATCH_V1") {
  const auto result = executor::ValidateModelFamilySourceInputV1(input);
  return !result.accepted && result.diagnostic_id == diagnostic;
}

}  // namespace

int main() {
  bool passed = true;
  std::uint16_t source_ordinal = 0;
  for (const auto family : {std::string("relational"),
                            std::string("document"),
                            std::string("graph")}) {
    auto single = Input(family, false);
    passed &= Require(
        executor::ValidateModelFamilySourceInputV1(single).accepted,
        family + " single-family empty timestamp was not preserved");
    single.mga_statement_context.statement_timestamp =
        "2026-08-12T20:00:00Z";
    passed &= Require(
        Refused(single, "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1"),
                      family + " single-family timestamp was accepted");

    auto common = Input(family, false);
    common.multimodel_common_statement_context = true;
    common.multimodel_composition_receipt_uuid = Uuid(100);
    common.multimodel_lexical_source_ordinal = source_ordinal++;
    common.multimodel_composition_arity = 3;
    passed &= Require(
        executor::ValidateModelFamilySourceInputV1(common).accepted,
        family + " common multimodel empty timestamp context was refused");
    auto timestamp_common = common;
    timestamp_common.mga_statement_context.statement_timestamp =
        "2026-08-12T20:00:00Z";
    passed &= Require(
        executor::ValidateModelFamilySourceInputV1(timestamp_common).accepted,
        family + " common multimodel canonical timestamp was refused");
    auto malformed_timestamp = timestamp_common;
    malformed_timestamp.mga_statement_context.statement_timestamp =
        "2026-02-30T25:61:61Z";
    passed &= Require(Refused(malformed_timestamp),
                      family + " malformed common timestamp was accepted");
    auto bad = common;
    bad.multimodel_composition_receipt_uuid.clear();
    passed &= Require(Refused(bad), family + " absent receipt was accepted");
    bad = common;
    bad.multimodel_lexical_source_ordinal = 3;
    passed &= Require(Refused(bad), family + " invalid ordinal was accepted");
    bad = common;
    bad.multimodel_composition_arity = 2;
    passed &= Require(Refused(bad), family + " invalid arity was accepted");
    bad = common;
    bad.multimodel_common_statement_context = false;
    passed &= Require(Refused(bad), family + " false common-context flag was accepted");
  }
  if (!passed) return 1;
  std::cout << "QOW-OPT-007-LEG: passed;families=3;timestamp_states=3;common_refusals=4\n";
  return 0;
}
