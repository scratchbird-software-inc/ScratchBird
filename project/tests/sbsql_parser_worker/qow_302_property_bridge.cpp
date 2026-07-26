// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

namespace {

constexpr std::string_view kCatalogEpochUuid =
    "019f0000-0000-7100-8000-000000003002";
constexpr std::string_view kSecurityContextUuid =
    "019f0000-0000-7110-8000-000000003003";
constexpr std::string_view kOrderingOneUuid =
    "019f0000-0000-7200-8000-000000003101";
constexpr std::string_view kGroupingUuid =
    "019f0000-0000-7200-8000-000000003102";
constexpr std::string_view kOrderingTwoUuid =
    "019f0000-0000-7200-8000-000000003103";
constexpr std::string_view kPartitioningUuid =
    "019f0000-0000-7200-8000-000000003104";
constexpr std::string_view kWindowUuid =
    "019f0000-0000-7200-8000-000000003105";
constexpr std::string_view kEquivalenceUuid =
    "019f0000-0000-7200-8000-000000003106";

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << "QOW-TEST-302-PROPERTY-BRIDGE-V1: "
                            << message << '\n';
  return condition;
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.local_transaction_id = 3001;
  context.snapshot_visible_through_local_transaction_id = 2999;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical = kCatalogEpochUuid;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      kSecurityContextUuid;
  return context;
}

sblr::SblrOperationEnvelope PropertyEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "qow.302.property.bridge");
  envelope.result_shape = "query_execute_result";
  envelope.requires_transaction_context = true;
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000003001"},
      {"uuid", "relational_catalog_epoch_uuid",
       std::string(kCatalogEpochUuid)},
      {"uuid", "relational_security_context_uuid",
       std::string(kSecurityContextUuid)},
      {"uint32", "relational_root_node_id", "4"},
      {"relational_descriptor_v1", "1",
       "019f0000-0000-7300-8000-000000003201|"
       "019f0000-0000-7400-8000-000000003202|1|-|-|-|-|-"},
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7300-8000-000000003203|"
       "019f0000-0000-7400-8000-000000003204|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
      {"relational_expression_v1", "2", "1|-|2|-|-|1|-|32"},
      {"relational_output_v1", "1", "1|1|1|1|0|67726f75705f6b6579"},
      {"relational_output_v1", "2", "1|2|2|1|1|6d656173757265"},
      {"relational_values_row_v1", "1", "1,2"},
      {"relational_node_v1", "1", "13|0|-|1,2|1"},
      {"relational_node_v1", "2", "5|0|1|1,2|-"},
      {"relational_node_v1", "3", "6|0|2|1,2|-"},
      {"relational_node_v1", "4", "8|0|3|1,2|-"},
      {"relational_node_binding_v1", "1",
       "76616c7565732e6c69746572616c2d7461626c652e7631|1,2|-|-|"
       "019f0000-0000-7200-8000-000000003101,"
       "019f0000-0000-7200-8000-000000003106"},
      {"relational_node_binding_v1", "2",
       "6167677265676174652e67726f75702e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000003101|"
       "019f0000-0000-7200-8000-000000003102"},
      {"relational_node_binding_v1", "3",
       "6f72646572696e672e6c6f676963616c2e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000003103|"
       "019f0000-0000-7200-8000-000000003103"},
      {"relational_node_binding_v1", "4",
       "77696e646f772e6f7665722e7631|1,2|-|"
       "019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104|"
       "019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104,"
       "019f0000-0000-7200-8000-000000003105"},
      {"relational_property_v1", std::string(kOrderingOneUuid),
       "1|1|-|1:1:2:-|-|-"},
      {"relational_property_v1", std::string(kGroupingUuid),
       "2|2|1|-|-|-"},
      {"relational_property_v1", std::string(kOrderingTwoUuid),
       "1|3|-|2:2:1:-|-|-"},
      {"relational_property_v1", std::string(kPartitioningUuid),
       "3|4|1|-|-|-"},
      {"relational_property_v1", std::string(kWindowUuid),
       "4|4|-|-|019f0000-0000-7200-8000-000000003103,"
       "019f0000-0000-7200-8000-000000003104|"
       "019f0000-0000-7500-8000-000000003106"},
      {"relational_property_v1", std::string(kEquivalenceUuid),
       "5|1|1,2|-|-|-"},
  };
  return envelope;
}

bool ValidateLivePropertyPopulation() {
  const auto result = sblr::DispatchSblrOperation(
      {Context(), PropertyEnvelope(), {}});
  bool passed = true;
  passed &= Require(result.envelope_validated && result.accepted &&
                        result.dispatched_to_api,
                    "property-bearing SBLR did not reach the logical bridge");
  passed &= Require(result.logical_graph_populated &&
                        result.logical_properties_populated &&
                        result.logical_node_count == 4 &&
                        result.logical_property_count == 6,
                    "logical/property population evidence differs");
  passed &= Require(
      !result.api_result.ok &&
          HasApiDiagnostic(
              result, "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING"),
      "logical population selected or executed a physical plan");
  return passed;
}

bool ValidateEngineScopeAndPropertyRefusal() {
  auto stale_context = Context();
  stale_context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7100-8000-000000003999";
  const auto stale = sblr::DispatchSblrOperation(
      {std::move(stale_context), PropertyEnvelope(), {}});

  auto unknown_property = PropertyEnvelope();
  unknown_property.operands[17].value =
      "6167677265676174652e67726f75702e7631|1,2|-|"
      "019f0000-0000-7200-8000-000000003999|"
      "019f0000-0000-7200-8000-000000003102";
  const auto malformed = sblr::DispatchSblrOperation(
      {Context(), std::move(unknown_property), {}});

  bool passed = true;
  passed &= Require(!stale.accepted && !stale.logical_graph_populated &&
                        HasApiDiagnostic(
                            stale, "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
                    "stale engine metadata scope reached planning");
  passed &= Require(
      !malformed.accepted && !malformed.logical_properties_populated &&
          HasApiDiagnostic(
              malformed, "QOW-DIAG-LOGICAL-PROPERTY-REFERENCE-V1"),
      "unknown property reference reached canonical logical population");
  return passed;
}

}  // namespace

// QOW-TEST-302-PROPERTY-BRIDGE-V1
int main() {
  if (!ValidateLivePropertyPopulation() ||
      !ValidateEngineScopeAndPropertyRefusal()) {
    return EXIT_FAILURE;
  }
  std::cout << "QOW-TEST-302-PROPERTY-BRIDGE-V1: PASS\n";
  return EXIT_SUCCESS;
}
