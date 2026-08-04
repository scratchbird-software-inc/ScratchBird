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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace {

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'fe00ULL;
constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000002101";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-008-RUNTIME-BREADTH-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext() {
  return {
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7200-8000-000000002102",
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7200-8000-000000002104",
      kOwner,
      0,
      kOwner - 16,
      kOwner - 32,
      kOwner - 32,
      kOwner - 32,
      {kOwner - 16, kOwner},
      {kOwner - 8},
      "statement_stable",
      kOwner + 16,
      true,
      true,
      true,
  };
}

exec::CanonicalExecutionMgaAuthority BindPhysicalAbiV2(
    exec::TypedPhysicalNodeDag* dag) {
  dag->abi_version = 2;
  dag->local_transaction_id = kOwner;
  dag->bound_sblr_tree_uuid = dag->admission_evidence.at(0).evidence_uuid;
  dag->catalog_epoch_uuid = dag->admission_evidence.at(1).evidence_uuid;
  dag->security_context_uuid = dag->admission_evidence.at(2).evidence_uuid;
  dag->capability_snapshot_uuid = dag->admission_evidence.at(4).evidence_uuid;
  dag->resource_snapshot_uuid = dag->admission_evidence.at(5).evidence_uuid;
  dag->statistics_snapshot_uuid = dag->admission_evidence.at(6).evidence_uuid;
  dag->route_snapshot_uuid = dag->admission_evidence.at(7).evidence_uuid;
  dag->catalog_generation = 1;
  dag->security_epoch = 2;
  dag->policy_epoch = 3;
  dag->resource_epoch = 4;
  dag->statistics_generation = 5;
  dag->route_epoch = 6;
  dag->route_generation = 7;
  dag->memory_budget_bytes = 64 * 1024;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  auto context = StatementContext();
  context.statement_snapshot_uuid = dag->admission_evidence.at(3).evidence_uuid;
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-000000002111";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-000000002112";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-000000002113";
    node.memory_bytes_required = 1;
    node.engine_capability_validated = true;
  }
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return authority;
}

api::EngineDescriptor Descriptor(const std::uint32_t ordinal,
                                 std::string type,
                                 std::string profile) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-0000000022" +
      (ordinal < 10 ? std::string("0") : std::string{}) +
      std::to_string(ordinal);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type);
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-0000000022" +
      (ordinal < 10 ? std::string("0") : std::string{}) +
      std::to_string(ordinal) + ";" + std::move(profile);
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

dt::DatatypeTextSeedAuthority TextSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow_core_resource_catalog";
  seed.seed_pack_version = "2026.08";
  seed.charset_name = "UTF-8";
  seed.collation_name = "unicode_ci_ai";
  seed.collation_case_insensitive = true;
  seed.collation_accent_insensitive = true;
  return seed;
}

dt::TimezoneSeedAuthority TimezoneSeed() {
  dt::TimezoneSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow_core_resource_catalog";
  seed.seed_pack_version = "2026.08";
  seed.content_hash = "sha256:qow-runtime-breadth-timezone";
  seed.timezone_records = 2;
  seed.timezone_transition_records = 100;
  seed.timezone_leap_second_records = 27;
  seed.timezone_names = {"America/Toronto", "Etc/UTC"};
  return seed;
}

struct Fixture {
  exec::TypedPhysicalNodeDag dag;
  exec::CanonicalExecutionMgaAuthority authority;
  exec::DescriptorBatch input;
  std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms;
  std::vector<exec::CanonicalDescriptorOrderTerm> order_terms;
};

Fixture MakeFixture() {
  const auto int8 = Descriptor(1, "int8", "nullability=non_null;width=8");
  const auto real64 =
      Descriptor(2, "real64", "nullability=non_null;width=64");
  const auto decimal = Descriptor(
      3, "decimal", "nullability=non_null;precision=6;scale=2");
  const auto binary =
      Descriptor(4, "binary", "nullability=non_null;width=16");
  const auto uuid = Descriptor(5, "uuid", "nullability=non_null;width=128");
  const auto timestamp = Descriptor(
      6, "timestamp",
      "nullability=non_null;timezone_profile_id="
      "timestamp_timezone_profile;precision=6");
  const auto text = Descriptor(
      7, "text", "nullability=nullable;collation_uuid=" +
                     std::string(kCollationUuid));
  const std::vector<std::uint32_t> descriptor_ids =
      {2101, 2102, 2103, 2104, 2105, 2106, 2107};

  Fixture fixture;
  fixture.dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002120";
  fixture.dag.root_physical_node_id = 2104;
  fixture.dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002121"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002122"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002123"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002124"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002125"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002126"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002127"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002128"},
  };
  fixture.dag.nodes = {
      {.physical_node_id = 2101,
       .relational_node_id = 2101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.materialize.canonical.v1",
       .output_descriptor_ids = descriptor_ids,
       .causal_counter_id = 21101},
      {.physical_node_id = 2102,
       .relational_node_id = 2102,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.query-distinct.typed.v1",
       .input_physical_node_ids = {2101},
       .output_descriptor_ids = descriptor_ids,
       .causal_counter_id = 21102},
      {.physical_node_id = 2103,
       .relational_node_id = 2103,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.terms.v1",
       .input_physical_node_ids = {2102},
       .output_descriptor_ids = descriptor_ids,
       .causal_counter_id = 21103},
      {.physical_node_id = 2104,
       .relational_node_id = 2104,
       .node_kind = exec::PhysicalNodeKind::kLimit,
       .implementation_id = "limit.typed.v1",
       .input_physical_node_ids = {2103},
       .output_descriptor_ids = descriptor_ids,
       .causal_counter_id = 21104},
  };
  fixture.authority = BindPhysicalAbiV2(&fixture.dag);
  fixture.input = exec::MakeDescriptorBatch(
      {{"tiny", int8, false, 2101},
       {"ratio", real64, false, 2102},
       {"amount", decimal, false, 2103},
       {"payload", binary, false, 2104},
       {"identity", uuid, false, 2105},
       {"observed_at", timestamp, false, 2106},
       {"label", text, true, 2107}},
      {{{Value(int8, "-1"), Value(real64, "2.5"),
         Value(decimal, "10.00"), Value(binary, std::string("a\0", 2)),
         Value(uuid, "019f0000-0000-7200-8000-000000002201"),
         Value(timestamp, "2026-08-03T09:00:00-04:00"),
         Value(text, "R\xC3\xA9sum\xC3\xA9")}},
       {{Value(int8, "-1"), Value(real64, "2.5"),
         Value(decimal, "10.00"), Value(binary, std::string("a\0", 2)),
         Value(uuid, "019f0000-0000-7200-8000-000000002201"),
         Value(timestamp, "2026-08-03T09:00:00-04:00"),
         Value(text, "resume")}},
       {{Value(int8, "0"), Value(real64, "1.25"),
         Value(decimal, "9.50"), Value(binary, std::string("b\0", 2)),
         Value(uuid, "019f0000-0000-7200-8000-000000002202"),
         Value(timestamp, "2026-08-03T10:00:00-04:00"), Null(text)}},
       {{Value(int8, "1"), Value(real64, "3.75"),
         Value(decimal, "11.25"), Value(binary, std::string("c\0", 2)),
         Value(uuid, "019f0000-0000-7200-8000-000000002203"),
         Value(timestamp, "2026-08-03T11:00:00 America/Toronto"),
         Value(text, "Zulu")}}});

  for (std::size_t column = 0; column < descriptor_ids.size(); ++column) {
    exec::CanonicalDescriptorOrderTerm term;
    term.column = column;
    term.expression_descriptor_id = descriptor_ids[column];
    term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
    if (column == 5) {
      term.resource_epoch = 51;
      term.timezone_epoch = 52;
      term.timezone_seed = TimezoneSeed();
    } else if (column == 6) {
      term.collation_uuid = kCollationUuid;
      term.resource_epoch = 41;
      term.collation_epoch = 42;
      term.text_seed = TextSeed();
    }
    fixture.equality_terms.push_back(term);
    term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
    fixture.order_terms.push_back(std::move(term));
  }
  return fixture;
}

bool ValidateBreadthComposition() {
  auto fixture = MakeFixture();
  exec::CanonicalDescriptorDistinctRequest distinct;
  distinct.physical_dag = fixture.dag;
  distinct.selected_physical_node_id = 2102;
  distinct.input_batch = fixture.input;
  distinct.equality_terms = fixture.equality_terms;
  distinct.maximum_value_comparisons = 1024;
  distinct.mga_authority = fixture.authority;
  const auto distinct_result = exec::ExecuteCanonicalDescriptorDistinct(
      distinct);
  if (!distinct_result.diagnostic.ok) {
    std::cerr << distinct_result.diagnostic.diagnostic_code << ": "
              << distinct_result.diagnostic.detail << '\n';
  }
  bool passed = true;
  passed &= Require(distinct_result.diagnostic.ok &&
                        distinct_result.output_batch.rows.size() == 3 &&
                        distinct_result.eliminated_duplicate_row_count == 1 &&
                        distinct_result.executed_physical_node_id == 2102,
                    "typed DISTINCT did not cover every descriptor family");

  exec::CanonicalDescriptorSortRequest sort;
  sort.physical_dag = fixture.dag;
  sort.selected_physical_node_id = 2103;
  sort.input_batch = distinct_result.output_batch;
  sort.order_terms = fixture.order_terms;
  sort.deterministic_tie_evidence_uuid =
      "019f0000-0000-7200-8000-000000002130";
  sort.maximum_pair_comparisons = 1024;
  sort.mga_authority = fixture.authority;
  const auto sort_result = exec::ExecuteCanonicalDescriptorSort(sort);
  if (!sort_result.diagnostic.ok) {
    std::cerr << sort_result.diagnostic.diagnostic_code << ": "
              << sort_result.diagnostic.detail << '\n';
  }
  passed &= Require(sort_result.diagnostic.ok &&
                        sort_result.output_batch.rows.size() == 3 &&
                        sort_result.output_batch.rows[0]
                                .values[0]
                                .encoded_value == "-1" &&
                        sort_result.output_batch.rows[2]
                                .values[0]
                                .encoded_value == "1" &&
                        sort_result.executed_physical_node_id == 2103,
                    "typed ORDER BY did not retain deterministic breadth");

  exec::CanonicalDescriptorLimitRequest limit;
  limit.physical_dag = fixture.dag;
  limit.selected_physical_node_id = 2104;
  limit.input_batch = sort_result.output_batch;
  limit.limit = 1;
  limit.offset = 1;
  limit.mga_authority = fixture.authority;
  const auto limit_result = exec::ExecuteCanonicalDescriptorLimit(limit);
  passed &= Require(limit_result.diagnostic.ok &&
                        limit_result.output_batch.rows.size() == 1 &&
                        limit_result.output_batch.rows[0]
                                .values[0]
                                .encoded_value == "0" &&
                        limit_result.executed_physical_node_id == 2104 &&
                        exec::PhysicalMgaStatementContextEqual(
                            limit_result.mga_statement_context,
                            fixture.authority.statement_context),
                    "typed OFFSET/LIMIT lost value or MGA authority");
  return passed;
}

bool ValidateFiniteScalarMatrix() {
  struct ScalarCase {
    std::string type;
    std::string profile;
    std::string encoded_value;
    bool sql_null{false};
  };
  const std::vector<ScalarCase> cases = {
      {"boolean", "nullability=non_null", "true"},
      {"int8", "nullability=non_null;width=8", "-128"},
      {"int16", "nullability=non_null;width=16", "-32768"},
      {"int32", "nullability=non_null;width=32", "-2147483648"},
      {"int64", "nullability=non_null;width=64", "-9223372036854775808"},
      {"int128", "nullability=non_null;width=128",
       "170141183460469231731687303715884105727"},
      {"real32", "nullability=non_null;width=32", "-3.5"},
      {"real64", "nullability=non_null;width=64", "2.5"},
      {"real128", "nullability=non_null;width=128", "1.25"},
      {"decimal", "nullability=non_null;precision=6;scale=2", "12.34"},
      {"decimal_float", "nullability=non_null;precision=8;scale=2",
       "1.25E+2"},
      {"binary", "nullability=non_null;width=16", std::string("b\0", 2)},
      {"uuid", "nullability=non_null;width=128",
       "019f0000-0000-7200-8000-000000002241"},
      {"date", "nullability=non_null", "2026-08-03"},
      {"time", "nullability=non_null;precision=6", "09:30:00.125"},
      {"timestamp", "nullability=non_null;precision=6",
       "2026-08-03T09:30:00.125"},
      {"interval", "nullability=non_null;width=128", "P1DT2H"},
      {"text", "nullability=nullable;collation_uuid=" +
                   std::string(kCollationUuid),
       {}, true},
  };

  std::vector<exec::ExecutorColumnDescriptor> columns;
  exec::DescriptorTuple row;
  columns.reserve(cases.size());
  row.values.reserve(cases.size());
  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto ordinal = static_cast<std::uint32_t>(index + 20);
    auto descriptor =
        Descriptor(ordinal, cases[index].type, cases[index].profile);
    columns.push_back({"scalar_" + std::to_string(index), descriptor,
                       cases[index].sql_null, 2200 + ordinal});
    row.values.push_back(cases[index].sql_null
                             ? Null(descriptor)
                             : Value(descriptor, cases[index].encoded_value));
  }
  const auto batch =
      exec::MakeDescriptorBatch(std::move(columns), {{std::move(row)}});
  const auto validation = exec::ValidateDescriptorBatch(batch);
  if (!validation.ok) {
    std::cerr << validation.diagnostic_code << ": " << validation.detail
              << " at column " << validation.column_index << '\n';
    return Require(false, "finite scalar descriptor matrix was refused");
  }

  bool passed = true;
  for (std::size_t index = 0; index < batch.columns.size(); ++index) {
    exec::CanonicalDescriptorOrderTerm term;
    term.column = index;
    term.expression_descriptor_id = batch.columns[index].descriptor_id;
    term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
    if (cases[index].type == "text") {
      term.collation_uuid = kCollationUuid;
      term.resource_epoch = 61;
      term.collation_epoch = 62;
      term.text_seed = TextSeed();
    }
    const auto term_validation = exec::ValidateCanonicalDescriptorOrderTerm(
        term, batch.columns[index]);
    const auto compared = exec::CompareCanonicalDescriptorOrderValues(
        batch.rows[0].values[index], batch.rows[0].values[index], term);
    passed &= Require(term_validation.ok && compared.diagnostic.ok &&
                          compared.comparison == 0,
                      "finite scalar comparator matrix lost a datatype");
  }
  const auto float8 = Descriptor(
      80, "float8", "nullability=non_null;width=64");
  const auto legacy_alias = exec::MakeDescriptorBatch(
      {{"legacy_float8", float8, false, 2280}},
      {{{Value(float8, "-2.25")}}});
  passed &= Require(exec::ValidateDescriptorBatch(legacy_alias).ok,
                    "legacy float8 descriptor alias was not preserved");
  std::vector<exec::ExecutorColumnDescriptor> opaque_columns;
  exec::DescriptorTuple opaque_row;
  const std::vector<std::string> opaque_types = {
      "vector", "document", "json", "graph", "search"};
  for (std::size_t index = 0; index < opaque_types.size(); ++index) {
    const auto ordinal = static_cast<std::uint32_t>(90 + index);
    auto descriptor = Descriptor(
        ordinal, opaque_types[index], "nullability=non_null");
    opaque_columns.push_back(
        {"opaque_" + opaque_types[index], descriptor, false, 2290 + ordinal});
    opaque_row.values.push_back(Value(
        descriptor, "opaque:" + opaque_types[index]));
  }
  const auto opaque_batch = exec::MakeDescriptorBatch(
      std::move(opaque_columns), {{std::move(opaque_row)}});
  passed &= Require(exec::ValidateDescriptorBatch(opaque_batch).ok,
                    "pre-existing advanced opaque descriptors regressed");
  return passed;
}

bool ValidateAtomicRefusals() {
  auto overflow = MakeFixture();
  overflow.input.rows[0].values[0].encoded_value = "128";
  exec::CanonicalDescriptorDistinctRequest overflow_request;
  overflow_request.physical_dag = overflow.dag;
  overflow_request.selected_physical_node_id = 2102;
  overflow_request.input_batch = overflow.input;
  overflow_request.equality_terms = overflow.equality_terms;
  overflow_request.maximum_value_comparisons = 1024;
  overflow_request.mga_authority = overflow.authority;
  const auto overflow_result = exec::ExecuteCanonicalDescriptorDistinct(
      overflow_request);

  auto missing_timezone = MakeFixture();
  missing_timezone.equality_terms[5].resource_epoch = 0;
  missing_timezone.equality_terms[5].timezone_epoch = 0;
  missing_timezone.equality_terms[5].timezone_seed = {};
  exec::CanonicalDescriptorDistinctRequest timezone_request;
  timezone_request.physical_dag = missing_timezone.dag;
  timezone_request.selected_physical_node_id = 2102;
  timezone_request.input_batch = missing_timezone.input;
  timezone_request.equality_terms = missing_timezone.equality_terms;
  timezone_request.maximum_value_comparisons = 1024;
  timezone_request.mga_authority = missing_timezone.authority;
  const auto timezone_result = exec::ExecuteCanonicalDescriptorDistinct(
      timezone_request);

  auto malformed_uuid = MakeFixture();
  malformed_uuid.input.rows[0].values[4].encoded_value = "not-a-uuid";
  const auto batch_validation = exec::ValidateDescriptorBatch(
      malformed_uuid.input);
  return Require(!overflow_result.diagnostic.ok &&
                     overflow_result.output_batch.rows.empty() &&
                     !timezone_result.diagnostic.ok &&
                     timezone_result.output_batch.rows.empty() &&
                     !batch_validation.ok &&
                     batch_validation.diagnostic_code ==
                         "QOW-DIAG-QRY-008-RUNTIME-BREADTH-REFUSAL-V1",
                 "overflow, timezone, or UUID refusal published a substitute");
}

}  // namespace

// QOW-TEST-QRY-008-RUNTIME-BREADTH-V1
int main() {
  const bool passed = ValidateBreadthComposition() &&
                      ValidateFiniteScalarMatrix() &&
                      ValidateAtomicRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
