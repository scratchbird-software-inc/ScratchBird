// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "optimizer_rewrite.hpp"

#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
using Uuid = scratchbird::core::platform::Uuid;
using scratchbird::tests::FixtureUuid;
using Refusal = opt::ProjectionPruneRefusal;
static_assert(!std::is_constructible_v<Uuid, const char*>);
static_assert(std::is_same_v<decltype(opt::ProjectionPruneInput{}.produced_column_uuids),
                             std::vector<Uuid>>);

namespace {
std::size_t checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << "native projection identity: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
bool V7(const Uuid& value) {
  return (value.bytes[6] & 0xf0) == 0x70 && (value.bytes[8] & 0xc0) == 0x80;
}
void Refused(const opt::RewriteDecision& result, Refusal reason) {
  Require(!result.applied && result.projection_refusal == reason, "exact refusal reason");
  Require(result.preserved_column_uuids.empty() && result.preserved_expression_term_ids.empty(),
          "refusal published partial projection or fabricated expression identity");
  Require(result.diagnostics == std::vector<std::string>{"SB-OPT-0001"},
          "registered optimizer refusal diagnostic");
}

void EveryByteAndCarrier() {
  const auto fixed = FixtureUuid(2206, 1);
  const auto original = FixtureUuid(2206, 99);
  const auto drop = FixtureUuid(88888, 1337);
  for (unsigned role = 0; role != 3; ++role) {
    for (unsigned position = 0; position != 16; ++position) {
      for (unsigned octet = 0; octet != 256; ++octet) {
        auto probe = original;
        probe.bytes[position] = static_cast<scratchbird::core::platform::byte>(octet);
        opt::ProjectionPruneInput input;
        std::vector<Uuid> expected;
        if (role == 0) {
          input = {{probe, drop}, {probe}, {}};
          expected = {probe};
        } else if (role == 1) {
          input = {{fixed, probe, drop}, {probe}, {fixed}};
          expected = {fixed, probe};
        } else {
          input = {{fixed, probe, drop}, {fixed}, {probe}};
          expected = {fixed, probe};
        }
        const auto original_input = input;
        const auto result = opt::PruneProjection(input);
        Require(input.produced_column_uuids == original_input.produced_column_uuids &&
                input.required_column_uuids == original_input.required_column_uuids &&
                input.masked_column_uuids == original_input.masked_column_uuids,
                "input changed during pruning or refusal");
        if (!V7(probe)) {
          Refused(result, Refusal::kInvalidIdentity);
          continue;
        }
        Require(result.applied && result.projection_refusal == Refusal::kNone,
                "valid native UUID failed projection pruning");
        Require(result.preserved_column_uuids == expected, "order/multiplicity/exact UUID bytes changed");
        Require(result.preserved_expression_term_ids.empty(), "column identity leaked into term labels");
        if (probe != original && probe != drop) {
          Refused(opt::PruneProjection({{original, drop}, {probe}, {}}),
                  Refusal::kUnboundRequiredColumn);
          Refused(opt::PruneProjection({{original, drop}, {}, {probe}}),
                  Refusal::kUnboundMaskedColumn);
        }
      }
    }
  }
  Refused(opt::PruneProjection({{{}}, {}, {}}), Refusal::kInvalidIdentity);
  Refused(opt::PruneProjection({{fixed}, {{}}, {}}), Refusal::kInvalidIdentity);
  Refused(opt::PruneProjection({{fixed}, {}, {{}}}), Refusal::kInvalidIdentity);
}

void OccurrencesAndBindings() {
  const auto a = FixtureUuid(2206, 1), b = FixtureUuid(2206, 2), c = FixtureUuid(2206, 3);
  const auto retained = opt::PruneProjection({{b, a, b, c, a}, {a, a}, {b, b}});
  Require(retained.applied && retained.preserved_column_uuids == std::vector<Uuid>{b, a, b, a},
          "repeated projected fields were deduplicated or reordered");
  const auto unchanged = opt::PruneProjection({{a, a, b}, {a}, {b}});
  Require(!unchanged.applied && unchanged.projection_refusal == Refusal::kNone &&
          unchanged.preserved_column_uuids == std::vector<Uuid>{a, a, b},
          "no-op projection changed occurrences");
  const auto empty = opt::PruneProjection({});
  Require(!empty.applied && empty.projection_refusal == Refusal::kNone &&
          empty.preserved_column_uuids.empty(), "empty projection invented identity");
  const auto all_removed = opt::PruneProjection({{a, b}, {}, {}});
  Require(all_removed.applied && all_removed.preserved_column_uuids.empty(),
          "unused columns not pruned");
  Refused(opt::PruneProjection({{}, {a}, {}}), Refusal::kUnboundRequiredColumn);
  Refused(opt::PruneProjection({{}, {}, {a}}), Refusal::kUnboundMaskedColumn);
  Refused(opt::PruneProjection({{a, b}, {a, c}, {b}}), Refusal::kUnboundRequiredColumn);
  Refused(opt::PruneProjection({{a, b}, {a}, {b, c}}), Refusal::kUnboundMaskedColumn);
  // Invalid identities anywhere must prevent publication, even after a valid
  // dependency prefix or in an otherwise unused produced column.
  Refused(opt::PruneProjection({{a, {}}, {a}, {}}), Refusal::kInvalidIdentity);
  Refused(opt::PruneProjection({{a, b}, {a, {}}, {b}}), Refusal::kInvalidIdentity);
  Refused(opt::PruneProjection({{a, b}, {a}, {b, {}}}), Refusal::kInvalidIdentity);
}

void LocalTermsAreNotColumnUuids() {
  opt::OptimizerBarrierInput barriers;
  barriers.security_context_present = true;
  barriers.grants_proven = true;
  opt::CommonSubexpressionReuseInput input;
  input.equivalence_proven = true;
  input.equivalence_proof_digest = "proof:local-term-reuse";
  input.estimated_recompute_cost = 100;
  input.estimated_reuse_cost = 10;
  opt::OptimizerExpressionTerm term;
  term.term_id = "expression.local.first";
  term.operator_id = "op.add";
  term.descriptor_digest = "descriptor:int64";
  term.input_term_ids = {"bound.input.a", "bound.input.b"};
  input.terms.push_back(term);
  term.term_id = "expression.local.second";
  input.terms.push_back(term);
  const auto result = opt::SelectCommonSubexpressionReuse(input, barriers);
  Require(result.applied && result.preserved_expression_term_ids ==
              std::vector<std::string>{"expression.local.first", "expression.local.second"},
          "CSE term identity/content lost during UUID conversion");
  Require(result.preserved_column_uuids.empty(), "expression labels impersonated system UUIDs");
}
}  // namespace

int main() {
  EveryByteAndCarrier();
  OccurrencesAndBindings();
  LocalTermsAreNotColumnUuids();
  std::cout << "native projection identity: " << checks << " checks PASS\n";
}
