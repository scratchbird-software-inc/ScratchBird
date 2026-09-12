// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/optimizer/optimizer_planning_context.hpp"
#include "binary_uuid_fixture.hpp"
#include "hash_digest.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
}
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace p = scratchbird::engine::planner;
namespace o = scratchbird::engine::optimizer;
namespace e = scratchbird::engine::executor;
using Uuid = p::CanonicalPlannerUuid;
static_assert(sizeof(Uuid) == 16);
static_assert(!std::is_constructible_v<Uuid, std::string>);
namespace {
unsigned checks = 0, faults = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
Uuid Id(unsigned value) {
  auto id = scratchbird::tests::BinaryUuid("019f0000-0000-7600-8000-000000000000");
  id.bytes[14] = value >> 8;
  id.bytes[15] = value;
  return id;
}
// Independent byte reader: field widths, lengths and order follow Core, not
// a round trip through the production writer or a shared serializer.
struct Reader {
  std::string_view bytes;
  std::size_t offset = 0;
  std::string_view Take(std::size_t size) {
    Check(size <= bytes.size() - offset, "truncated canonical field");
    auto result = bytes.substr(offset, size); offset += size; return result;
  }
  void N(std::uint64_t expected) {
    auto part = Take(8); std::uint64_t actual = 0;
    for (unsigned i = 0; i < 8; ++i)
      actual |= std::uint64_t(static_cast<unsigned char>(part[i])) << (8 * i);
    Check(actual == expected, "noncanonical integer/count");
  }
  void B(bool expected) { Check(Take(1)[0] == (expected ? 1 : 0), "flag encoding"); }
  void U(const Uuid& expected) {
    auto part = Take(16);
    for (unsigned i = 0; i < 16; ++i)
      Check(static_cast<unsigned char>(part[i]) == expected.bytes[i], "binary identity bytes");
  }
  void T(std::string_view expected) {
    N(expected.size()); Check(Take(expected.size()) == expected, "framed field bytes");
  }
  void End() { Check(offset == bytes.size(), "unconsumed canonical bytes"); }
};
p::CanonicalMgaStatementContext Mga() {
  p::CanonicalMgaStatementContext m;
  m.statement_uuid = Id(11); m.owning_transaction_uuid = Id(12);
  m.statement_snapshot_uuid = Id(13); m.statement_metadata_snapshot_uuid = Id(14);
  m.owning_local_transaction_id = 20; m.visible_committed_high_watermark = 17;
  m.oldest_active_transaction_id = 18; m.oldest_interesting_transaction_id = 15;
  m.oldest_snapshot_transaction_id = 15; m.retention_horizon_transaction_id = 15;
  m.active_excluded_local_transaction_ids = {18, 20};
  m.in_doubt_excluded_local_transaction_ids = {19};
  m.snapshot_kind = "statement_stable";
  m.publication_inventory_next_local_transaction_id = 21;
  m.inventory_authoritative = m.complete = m.current = true;
  m.statement_timestamp = "2026-09-12T12:00:00Z";
  return m;
}
struct Fixture {
  p::CanonicalLogicalRelationalGraph graph;
  p::CanonicalLogicalPropertyCatalog properties;
  o::CanonicalPlannerContinuationContext context;
  Fixture() {
    graph.bound_sblr_tree_uuid = Id(1); graph.catalog_epoch_uuid = Id(2);
    graph.security_context_uuid = Id(3); graph.mga_statement_context = Mga();
    graph.local_transaction_id = 20; graph.statement_snapshot_id = 17;
    graph.root_logical_node_id = 1; graph.result_descriptor_ids = {100};
    p::CanonicalLogicalRelationalNode node;
    node.logical_node_id = 1; node.node_kind = p::CanonicalLogicalRelationalNodeKind::kValues;
    node.semantic_variant_id = "values.literal-table.v1";
    node.output_descriptor_ids = {100}; node.bound_expression_ids = {101, 102};
    node.origin_relational_node_ids = {1};
    node.required_property_uuids = node.delivered_property_uuids = {Id(31), Id(32), Id(33)};
    graph.nodes = {node};
    properties.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
    properties.catalog_epoch_uuid = graph.catalog_epoch_uuid;
    properties.security_context_uuid = graph.security_context_uuid;
    properties.mga_statement_context = graph.mga_statement_context;
    properties.local_transaction_id = 20; properties.statement_snapshot_id = 17;
    p::CanonicalLogicalPropertyRecord ordering;
    ordering.property_uuid = Id(31); ordering.origin_logical_node_id = 1;
    ordering.populated_from_bound_sblr = true;
    ordering.ordering_terms = {{101, p::CanonicalLogicalPropertySortDirection::kAscending,
                               p::CanonicalLogicalPropertyNullPlacement::kNullsLast, Id(34)}};
    auto material = p::CanonicalLogicalPropertyRecord{};
    material.property_uuid = Id(32); material.origin_logical_node_id = 1;
    material.populated_from_bound_sblr = true;
    material.property_kind = p::CanonicalLogicalPropertyKind::kMaterialization;
    material.materialization_kind = p::CanonicalLogicalMaterializationKind::kMaterialized;
    auto rewind = p::CanonicalLogicalPropertyRecord{};
    rewind.property_uuid = Id(33); rewind.origin_logical_node_id = 1;
    rewind.populated_from_bound_sblr = true;
    rewind.property_kind = p::CanonicalLogicalPropertyKind::kRewindability;
    rewind.rewindability_kind = p::CanonicalLogicalRewindabilityKind::kRewindable;
    properties.properties = {ordering, material, rewind};
    context.authority.context_uuid = Id(41); context.authority.generation = 2;
    context.authority.authority_uuid = Id(42); context.authority.authority_generation = 3;
    context.authority.confidence_basis_points = 9500;
    context.authority.dependency_signature = std::string(64, 'a');
    context.authority.invalid_state_behavior_id = "reject_continuation_plan";
    context.authority.engine_owned = true;
    context.prepared_statement_uuid = Id(43); context.prepared_statement_generation = 4;
    context.cursor_uuid = Id(44); context.cursor_generation = 5;
    context.continuation_token_uuid = Id(45); context.continuation_token_generation = 6;
    context.resume_boundary_uuid = Id(46); context.resume_boundary_generation = 7;
    context.result_schema_uuid = Id(47);
    context.required_ordering_property_uuid = Id(31);
    context.required_materialization_property_uuid = Id(32);
    context.required_rewindability_property_uuid = Id(33);
    context.continuation_requested = true;
  }
  auto Validate() const {
    return o::ValidateCanonicalPlannerContexts(graph, properties, context, std::nullopt);
  }
};
void Authority(Reader& r, const o::CanonicalPlannerContextAuthority& a) {
  r.U(a.context_uuid); r.N(a.generation); r.U(a.authority_uuid); r.N(a.authority_generation);
  r.N(a.confidence_basis_points); r.T(a.dependency_signature); r.T(a.invalid_state_behavior_id);
  r.B(a.engine_owned); r.B(a.parser_execution_authority_claimed);
  r.B(a.transaction_visibility_authority_claimed); r.B(a.transaction_finality_authority_claimed);
  r.B(a.recovery_authority_claimed);
}
void VerifySerialization() {
  Fixture f;
  Check(p::ValidateCanonicalLogicalRelationalGraph(f.graph).accepted, "logical graph fixture");
  auto bytes = p::SerializeCanonicalMgaStatementContext(f.graph.mga_statement_context);
  Reader r{bytes}; r.T("mga-v2");
  for (unsigned id = 11; id < 15; ++id) r.U(Id(id));
  for (unsigned n : {20, 17, 18, 15, 15, 15, 2, 18, 20}) r.N(n);
  r.T("2026-09-12T12:00:00Z"); r.N(1); r.N(19); r.T("statement_stable"); r.N(21);
  r.B(true); r.B(true); r.B(true); r.End();
  auto property = f.properties.properties[0];
  auto prop = p::SerializeCanonicalLogicalPropertyIdentity(property);
  Reader q{prop}; q.T("logical-property-v2"); q.U(Id(31));
  for (unsigned n : {1, 1, 0, 1, 101, 1, 2}) q.N(n);
  q.U(Id(34)); q.N(0); q.U({});
  for (unsigned n = 0; n < 4; ++n) q.N(0);
  q.U({}); q.U({}); q.N(0); q.B(true); q.End();
  auto catalog = p::SerializeCanonicalLogicalPropertyCatalog(f.graph, f.properties);
  Check(catalog.accepted, "catalog serialization fixture");
  Reader c{catalog.canonical_serialization}; c.T("logical-property-catalog-v2"); c.N(1);
  c.U(Id(1)); c.U(Id(2)); c.U(Id(3)); c.N(20); c.N(17); c.T(bytes);
  c.B(false); c.B(false); c.B(false); c.N(3);
  for (const auto& item : f.properties.properties)
    c.T(p::SerializeCanonicalLogicalPropertyIdentity(item));
  c.N(1); c.N(1); c.N(3); c.U(Id(31)); c.U(Id(32)); c.U(Id(33));
  c.N(3); c.U(Id(31)); c.U(Id(32)); c.U(Id(33)); c.End();
  std::ranges::reverse(f.properties.properties);
  std::ranges::reverse(f.graph.nodes[0].required_property_uuids);
  std::ranges::reverse(f.graph.nodes[0].delivered_property_uuids);
  Check(p::SerializeCanonicalLogicalPropertyCatalog(f.graph, f.properties).canonical_serialization ==
        catalog.canonical_serialization, "semantic set ordering must not change binding");
  property.property_kind = p::CanonicalLogicalPropertyKind::kGrouping;
  property.ordering_terms.clear(); property.expression_ids = {102, 101};
  auto group = p::SerializeCanonicalLogicalPropertyIdentity(property);
  std::ranges::reverse(property.expression_ids);
  Check(group == p::SerializeCanonicalLogicalPropertyIdentity(property), "group expression set order");
  property = Fixture{}.properties.properties[0];
  property.ordering_terms.push_back({102, p::CanonicalLogicalPropertySortDirection::kDescending,
      p::CanonicalLogicalPropertyNullPlacement::kNullsFirst, Id(35)});
  auto ordered = p::SerializeCanonicalLogicalPropertyIdentity(property);
  std::ranges::reverse(property.ordering_terms);
  Check(ordered != p::SerializeCanonicalLogicalPropertyIdentity(property), "ordered terms remain ordered");
  auto m = Mga(); m.statement_timestamp = std::string("x\0y:;", 5);
  auto binary = p::SerializeCanonicalMgaStatementContext(m);
  m.statement_timestamp = "x";
  Check(binary != p::SerializeCanonicalMgaStatementContext(m), "embedded nul must not truncate");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = Fixture{}.properties.properties[0];
    changed.property_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(prop != p::SerializeCanonicalLogicalPropertyIdentity(changed), "all property UUID bits bind");
    auto changed_mga = Mga(); changed_mga.statement_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    Check(bytes != p::SerializeCanonicalMgaStatementContext(changed_mga), "all statement UUID bits bind");
  }
}
o::CanonicalPlannerContinuationReplayRequest Replay(const Fixture& f) {
  o::CanonicalPlannerContinuationReplayRequest r;
  r.context = f.context;
  r.engine_replay_authorized = r.cursor_state_revalidated = r.continuation_state_revalidated = true;
  return r;
}
void VerifyReplay() {
  Fixture f; auto validation = f.Validate();
  Check(validation.accepted && validation.continuation_receipt.has_value(), "continuation admission");
  auto expected = *validation.continuation_receipt; auto replay = Replay(f);
  o::CanonicalPlannerContinuationReplayReceipt receipt;
  Check(o::CanonicalPlannerContinuationReplayMatches(expected, replay, &receipt), "matching replay");
  Check(receipt.identity_revalidated && !receipt.consumed_once, "match is not replay consumption");
  Reader r{receipt.replay_identity}; r.T("planner-continuation-replay-v2"); r.N(1);
  Authority(r, f.context.authority);
  for (unsigned id = 43; id <= 46; ++id) { r.U(Id(id)); r.N(id - 39); }
  r.U(Id(47)); r.U(Id(31)); r.U(Id(32)); r.U(Id(33));
  r.N(1); r.N(1); r.B(true); r.B(true); r.End();
  const auto members = {&o::CanonicalPlannerContinuationContext::prepared_statement_uuid,
    &o::CanonicalPlannerContinuationContext::cursor_uuid,
    &o::CanonicalPlannerContinuationContext::continuation_token_uuid,
    &o::CanonicalPlannerContinuationContext::resume_boundary_uuid,
    &o::CanonicalPlannerContinuationContext::result_schema_uuid,
    &o::CanonicalPlannerContinuationContext::required_ordering_property_uuid,
    &o::CanonicalPlannerContinuationContext::required_materialization_property_uuid,
    &o::CanonicalPlannerContinuationContext::required_rewindability_property_uuid};
  for (auto member : members) for (unsigned bit = 0; bit < 128; ++bit) {
    auto mutated = replay;
    (mutated.context.*member).bytes[bit / 8] ^= 1U << (bit % 8);
    auto stale = receipt; stale.consumed_once = true;
    Check(!o::CanonicalPlannerContinuationReplayMatches(expected, mutated, &stale), "UUID replay mismatch");
    Check(stale == o::CanonicalPlannerContinuationReplayReceipt{}, "failed replay clears all stale output");
  }
  auto mutate = [&](auto change) {
    auto changed = replay; change(changed.context);
    auto stale = receipt; stale.consumed_once = true;
    Check(!o::CanonicalPlannerContinuationReplayMatches(expected, changed, &stale), "context mismatch");
    Check(stale == o::CanonicalPlannerContinuationReplayReceipt{}, "context failure atomicity");
    auto changed_expected = expected; changed_expected.context = changed.context;
    if (o::CanonicalPlannerContinuationReceiptValid(changed_expected)) {
      Check(o::CanonicalPlannerContinuationReplayMatches(changed_expected, changed, &stale), "changed matching context");
      Check(stale.replay_identity != receipt.replay_identity, "complete replay key binds context change");
    }
  };
  mutate([](auto& c) { ++c.authority.generation; });
  mutate([](auto& c) { c.authority.authority_uuid = Id(99); });
  mutate([](auto& c) { ++c.authority.authority_generation; });
  mutate([](auto& c) { --c.authority.confidence_basis_points; });
  mutate([](auto& c) { c.authority.dependency_signature[5] = 'b'; });
  mutate([](auto& c) { c.result_schema_uuid = Id(98); });
  mutate([](auto& c) { c.cursor_mode = o::CanonicalPlannerCursorMode::kScrollable; });
  mutate([](auto& c) { c.holdability = o::CanonicalPlannerCursorHoldability::kHoldable; });
  mutate([](auto& c) { c.authority.parser_execution_authority_claimed = true; });
  mutate([](auto& c) { c.continuation_requested = false; });
  e::TypedPhysicalNodeDag dag;
  dag.bound_sblr_tree_uuid = f.graph.bound_sblr_tree_uuid; dag.root_physical_node_id = 1;
  dag.optimizer_published = dag.immutable_node_identity_validated = dag.property_contract_validated = true;
  e::PhysicalNodeRecord node; node.physical_node_id = node.relational_node_id = 1;
  node.delivered_property_uuids = {Id(31), Id(32), Id(33)}; dag.nodes = {node};
  Check(o::ValidateCanonicalContinuationPhysicalRoot(&expected, dag), "physical root property match");
  dag.nodes[0].delivered_property_uuids.pop_back();
  Check(!o::ValidateCanonicalContinuationPhysicalRoot(&expected, dag), "missing physical delivery refuses");
  Check(!expected.physical_root_delivery_validated, "physical mismatch clears stale success");
}
o::CanonicalPlannerWhatIfContext WhatIf(const Fixture& f) {
  o::CanonicalPlannerWhatIfContext w; w.authority = f.context.authority;
  w.authority.invalid_state_behavior_id = "advisory_only_no_normal_plan_influence";
  w.policy_uuid = Id(51); w.policy_generation = 8; w.enabled = true;
  w.hypotheses = {{o::CanonicalPlannerWhatIfHypothesisKind::kIndex, Id(52), 9, std::string(64, 'b')}};
  return w;
}
void VerifyWhatIf() {
  Fixture f; auto w = WhatIf(f);
  auto validate = [&](const auto& context) {
    return o::ValidateCanonicalPlannerContexts(f.graph, f.properties, std::nullopt, context);
  };
  auto result = validate(w);
  Check(result.accepted && result.what_if_receipt.has_value(), "what-if advisory validation");
  Check(!result.physical_publication_allowed && !result.cache_admission_allowed && !result.execution_allowed,
        "what-if must not publish executable authority");
  const auto digest = result.what_if_receipt->hypothesis_set_digest;
  Check(digest == "af88b6eea0a1ef8940eb638e0c4cf796ede8153554173d94640875a38db57c21",
        "independent Core what-if digest vector");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = w; changed.hypotheses[0].hypothesis_uuid.bytes[bit / 8] ^= 1U << (bit % 8);
    auto next = validate(changed);
    Check(!next.accepted || next.what_if_receipt->hypothesis_set_digest != digest,
          "all hypothesis UUID bits bind or fail v7 validation");
  }
  w.hypotheses.push_back(w.hypotheses.front());
  Check(!validate(w).accepted, "duplicate hypothesis rejected");
  w = WhatIf(f); w.normal_plan_influence_permitted = true;
  Check(!validate(w).accepted, "advisory cannot influence normal plan");
  Check(!o::ValidateCanonicalPlannerContexts(f.graph, f.properties, f.context, WhatIf(f)).accepted,
        "continuation and what-if isolation");
}
template<class Run> void Faults(Run run) {
  bool finished = false;
  for (long index = 0; index < 4096; ++index) {
    fault::remaining = index; fault::hit = false;
    bool accepted = false, leaked = false;
    try { accepted = run(); } catch (...) { leaked = true; }
    const bool hit = fault::hit; fault::remaining = -1;
    Check(!leaked, "allocation failure must not escape validation/publication");
    if (!hit) { Check(accepted, "unfaulted baseline succeeds"); finished = true; break; }
    ++faults; Check(!accepted, "allocation failure must not publish success");
  }
  Check(finished, "allocation sweep exhausted");
}
void VerifyFaults() {
  Fixture f; auto validation = f.Validate(); auto expected = *validation.continuation_receipt;
  auto replay = Replay(f); std::optional<o::CanonicalPlannerContinuationContext> context = f.context;
  std::optional<o::CanonicalPlannerWhatIfContext> what_if = WhatIf(f);
  Faults([&] {
    o::CanonicalPlannerContinuationReplayReceipt result;
    result.identity_revalidated = result.dependency_revalidated = result.consumed_once = true;
    const bool ok = o::CanonicalPlannerContinuationReplayMatches(expected, replay, &result);
    if (!ok && result != o::CanonicalPlannerContinuationReplayReceipt{}) throw 1;
    return ok;
  });
  Faults([&] {
    auto result = o::ValidateCanonicalPlannerContexts(f.graph, f.properties, context, std::nullopt);
    if (!result.accepted && (result.continuation_receipt || result.execution_allowed)) throw 1;
    return result.accepted;
  });
  Faults([&] {
    auto result = o::ValidateCanonicalPlannerContexts(f.graph, f.properties, std::nullopt, what_if);
    if (!result.accepted && (result.what_if_receipt || result.execution_allowed)) throw 1;
    return result.accepted;
  });
  Faults([&] {
    auto result = p::SerializeCanonicalLogicalPropertyCatalog(f.graph, f.properties);
    if (!result.accepted && !result.canonical_serialization.empty()) throw 1;
    return result.accepted;
  });
}
}
int main() {
  try {
    VerifySerialization(); VerifyReplay(); VerifyWhatIf(); VerifyFaults();
    std::cout << "PASS planner binary contexts checks=" << checks << " faults=" << faults << '\n';
  } catch (const std::exception& error) {
    fault::remaining = -1; std::cerr << error.what() << '\n'; return 1;
  }
}
