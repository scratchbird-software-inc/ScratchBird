// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_WIN_004_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_win_004.cpp"
#include "uuid.hpp"
#include "datatype_catalog_manifest.hpp"
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

namespace {
using Receipt = exec::CanonicalWindowOrderBindingReceipt;
unsigned checks = 0, faults = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  exec::CanonicalWindowPartitionOrderRequest input = Window401Request();
  exec::CanonicalDescriptorOrderTerm term = input.order_terms.front();
  exec::ExecutorColumnDescriptor column = input.input_batch.columns[term.column];
  std::shared_ptr<exec::PhysicalMgaStatementContext> current =
      std::make_shared<exec::PhysicalMgaStatementContext>(input.physical_dag.mga_statement_context);
  Fixture() {
    input.physical_dag.nodes[0].delivered_property_uuids = {input.ordering_property_uuid};
    input.physical_dag.nodes[1].required_property_uuids = {input.ordering_property_uuid};
    input.mga_authority.resolve_current = [state = current] {
      exec::CanonicalMgaCurrentResolution result;
      result.statement_context = *state;
      return result;
    };
  }
  auto Issue(std::uint64_t limit = 16384) const {
    return Receipt::Issue(input.physical_dag, 2, column, term,
                          input.ordering_property_uuid, input.mga_authority, limit);
  }
  bool Matches(const Receipt& receipt, std::uint64_t* actual = nullptr) const {
    std::uint64_t ignored = 0;
    return receipt.Matches(input.physical_dag, 2, column, term,
                            input.ordering_property_uuid, input.mga_authority,
                            16384, actual ? actual : &ignored);
  }
};

void VerifyNtileExecution() {
  Fixture f;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "Core datatype manifest unavailable");
  api::EngineUuid int64_type;
  for (const auto& row : manifest.manifest.descriptor_rows) {
    if (row.stable_name != "int64") continue;
    for (const auto& codec : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
      if (codec.descriptor_uuid == row.descriptor_uuid.value &&
          codec.descriptor_generation == row.descriptor_epoch)
        int64_type = codec.type_uuid;
    }
  }
  Check(!int64_type.is_nil(), "Core int64 identity missing");
  f.column = {"ordered", WindowDescriptor(5000, "int64", int64_type,
                                         "nullability=non_null"), false, 4003};
  f.term.column = 0;
  auto& dag = f.input.physical_dag;
  dag.nodes[0].node_kind = exec::PhysicalNodeKind::kSort;
  dag.nodes[0].implementation_id = "sort.order.typed.v1";
  dag.nodes[0].output_descriptor_ids = {4003};
  dag.nodes[1].implementation_id = "window.ntile.v1";
  dag.nodes[1].output_descriptor_ids = {4003, 5001};
  dag.nodes[1].delivered_property_uuids = {
      f.input.ordering_property_uuid, f.input.window_property_uuid};
  exec::CanonicalDescriptorNtileRequest request;
  request.physical_dag = dag;
  request.selected_physical_node_id = 2;
  request.order_term = f.term;
  request.ntile_column = {"bucket", WindowDescriptor(5001, "int64", int64_type,
                                                   "nullability=non_null"), false, 5001};
  request.bucket_count_operand = WindowValue(
      WindowDescriptor(5002, "int64", int64_type, "nullability=non_null"), "3");
  request.function_abi_version = 1;
  request.builtin_id = "sb.window.ntile";
  request.function_uuid = scratchbird::tests::BinaryUuid(
      "019de5fc-2400-7047-9474-232ca488c094");
  request.deterministic_order_evidence_uuid = WindowUuid(5003);
  request.mga_authority = f.input.mga_authority;
  request.ordered_input_batch.columns = {f.column};
  for (unsigned row = 1; row <= 5; ++row)
    request.ordered_input_batch.rows.push_back({{WindowValue(f.column.descriptor, std::to_string(row))}});
  request.order_term_binding_receipt = f.Issue();
  Check(bool(request.order_term_binding_receipt), "NTILE receipt issuance failed");
  request.order_term_binding_evidence_uuid = request.order_term_binding_receipt->identity();
  auto result = exec::ExecuteCanonicalDescriptorNtile(request);
  if (!result.diagnostic.ok) std::cerr << result.diagnostic.detail << '\n';
  Check(result.diagnostic.ok && result.output_batch.rows.size() == 5,
        "NTILE with independent capability and receipt failed execution");
  for (unsigned row = 0; row < 5; ++row) {
    Check(result.output_batch.rows[row].values.back().encoded_value ==
              std::to_string(row / 2 + 1), "NTILE produced wrong bucket");
  }
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto changed = request;
    changed.order_term_binding_evidence_uuid.bytes[bit / 8] ^=
        static_cast<std::uint8_t>(1U << (bit % 8));
    result = exec::ExecuteCanonicalDescriptorNtile(changed);
    Check(!result.diagnostic.ok && result.output_batch.rows.empty() &&
              result.selected_plan_uuid.is_nil(), "forged receipt UUID published NTILE rows");
  }
  request.order_term_binding_receipt.reset();
  result = exec::ExecuteCanonicalDescriptorNtile(request);
  Check(!result.diagnostic.ok && result.output_batch.rows.empty(),
        "UUID without retained receipt published NTILE rows");
  for (bool dense : {false, true}) {
    exec::CanonicalDescriptorPeerRankingRequest peer;
    peer.physical_dag = dag;
    peer.physical_dag.nodes[1].implementation_id =
        dense ? "window.dense-rank.v1" : "window.rank.v1";
    peer.selected_physical_node_id = 2;
    peer.order_term = f.term;
    peer.ranking_column = request.ntile_column;
    peer.function_abi_version = 1;
    peer.builtin_id = dense ? "sb.window.dense_rank" : "sb.window.rank";
    peer.function_uuid = scratchbird::tests::BinaryUuid(
        dense ? "019de5fc-2400-741d-bef0-f079fd3ba494"
              : "019de5fc-2400-7b94-870d-0dd789ca70ab");
    peer.deterministic_order_evidence_uuid = WindowUuid(5003);
    peer.maximum_peer_comparisons = 16;
    peer.mga_authority = request.mga_authority;
    peer.ordered_input_batch.columns = {f.column};
    for (const char* value : {"1", "1", "3", "4", "4"})
      peer.ordered_input_batch.rows.push_back({{WindowValue(f.column.descriptor, value)}});
    peer.order_term_binding_receipt = Receipt::Issue(
        peer.physical_dag, 2, f.column, f.term, f.input.ordering_property_uuid,
        peer.mga_authority, 16384);
    Check(bool(peer.order_term_binding_receipt), "peer ranking receipt issuance failed");
    peer.order_term_binding_evidence_uuid = peer.order_term_binding_receipt->identity();
    const auto ranked = exec::ExecuteCanonicalDescriptorPeerRanking(peer);
    if (!ranked.diagnostic.ok) std::cerr << ranked.diagnostic.detail << '\n';
    Check(ranked.diagnostic.ok && ranked.output_batch.rows.size() == 5,
          "peer ranking with independent receipt failed execution");
    const std::array<const char*, 5> expected = dense
        ? std::array<const char*, 5>{"1", "1", "2", "3", "3"}
        : std::array<const char*, 5>{"1", "1", "3", "4", "4"};
    for (unsigned row = 0; row < 5; ++row)
      Check(ranked.output_batch.rows[row].values.back().encoded_value == expected[row],
            "peer ranking produced the wrong value");
  }
}

void Verify() {
  static_assert(!std::is_default_constructible_v<Receipt>);
  static_assert(!std::is_copy_constructible_v<Receipt>);
  Fixture source;
  const auto receipt = source.Issue();
  Check(receipt && source.Matches(*receipt), "valid retained binding refused");
  const auto second = source.Issue();
  Check(second && second->identity() != receipt->identity() &&
            second->digest() == receipt->digest(), "digest was reused as issued identity");
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(receipt->identity()),
        "receipt identity is not an issued v7");
  std::uint64_t expected = 0, actual = 0;
  Check(exec::PlanCanonicalDescriptorOrderTermBindingDigestWorkspace(
            source.term, source.input.ordering_property_uuid, &expected), "planning failed");
  const auto digest = exec::ComputeCanonicalDescriptorOrderTermBindingDigest(
      source.term, source.input.ordering_property_uuid, expected, &actual);
  Check(digest && *digest == receipt->digest() && actual == expected,
        "receipt did not retain complete content digest");
  // Independent Python hashlib/struct oracle over the specified v2 framing:
  // 228 content bytes, not the production encoder or hash helper.
  constexpr exec::CanonicalOrderTermBindingDigest literal_digest{
      0xc9,0xad,0xfb,0x32,0x54,0x8d,0x58,0xc5,0xb6,0xdd,0x14,0xaf,0xd1,0x9e,0x01,0xe3,
      0xd9,0x7d,0xc6,0x7d,0x64,0x67,0x5b,0x1e,0xdf,0x3a,0x75,0xf1,0x3a,0xdf,0x90,0xf6};
  Check(receipt->digest() == literal_digest && expected == 260,
        "order binding changed the independent canonical byte vector");
  Check(!source.Issue(expected - 1), "undersized workspace admitted receipt");
  Check(source.Issue(expected) != nullptr, "exact workspace was refused");

  for (auto slot : {&api::EngineDescriptor::descriptor_uuid,
                    &api::EngineDescriptor::type_uuid,
                    &api::EngineDescriptor::collation_uuid}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      Fixture changed;
      (changed.column.descriptor.*slot).bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1U << (bit % 8));
      actual = 99;
      Check(!changed.Matches(*receipt, &actual) && actual == 0,
            "changed descriptor binary identity matched retained receipt");
    }
  }
  for (auto slot : {&exec::TypedPhysicalNodeDag::selected_plan_uuid,
                    &exec::TypedPhysicalNodeDag::bound_sblr_tree_uuid,
                    &exec::TypedPhysicalNodeDag::catalog_epoch_uuid,
                    &exec::TypedPhysicalNodeDag::security_context_uuid,
                    &exec::TypedPhysicalNodeDag::capability_snapshot_uuid,
                    &exec::TypedPhysicalNodeDag::resource_snapshot_uuid,
                    &exec::TypedPhysicalNodeDag::statistics_snapshot_uuid,
                    &exec::TypedPhysicalNodeDag::route_snapshot_uuid}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      Fixture changed;
      (changed.input.physical_dag.*slot).bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1U << (bit % 8));
      Check(!changed.Matches(*receipt), "changed plan authority matched receipt");
    }
  }
  const auto reject = [&](auto change) {
    Fixture changed;
    change(changed);
    Check(!changed.Matches(*receipt), "changed binding matched retained receipt");
  };
  reject([](auto& f) { f.column.descriptor.encoded_descriptor += ";extra=1"; });
  reject([](auto& f) { f.column.nullable = !f.column.nullable; });
  reject([](auto& f) { f.column.stable_name += "changed"; });
  reject([](auto& f) { ++f.column.descriptor_id; });
  reject([](auto& f) { f.term.direction = exec::CanonicalDescriptorOrderDirection::descending; });
  reject([](auto& f) { f.term.null_placement = exec::CanonicalDescriptorNullPlacement::first; });
  reject([](auto& f) { ++f.term.column; });
  reject([](auto& f) { ++f.term.expression_descriptor_id; });
  reject([](auto& f) { ++f.input.physical_dag.nodes[1].causal_counter_id; });
  reject([](auto& f) { ++f.input.physical_dag.nodes[1].memory_bytes_required; });
  reject([](auto& f) { f.input.physical_dag.nodes[1].implementation_id += "changed"; });
  reject([](auto& f) { f.input.physical_dag.nodes[1].executor_capability_uuid.bytes[15] ^= 1; });
  reject([](auto& f) { ++f.input.physical_dag.resource_epoch; });
  reject([](auto& f) { f.input.physical_dag.mga_statement_context.statement_timestamp = "changed"; });
  reject([](auto& f) { f.input.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing; });

  // Even a replacement resolver returning the original statement cannot hide
  // expiration observed by the independently retained issuance resolver.
  source.current->statement_uuid.bytes[15] ^= 1;
  const auto original = source.input.mga_authority.statement_context;
  source.input.mga_authority.resolve_current = [original] {
    exec::CanonicalMgaCurrentResolution r; r.statement_context = original; return r;
  };
  Check(!source.Matches(*receipt), "swapped resolver revived stale statement receipt");
  source.current->statement_uuid.bytes[15] ^= 1;
  Check(source.Matches(*receipt), "restored statement did not match retained receipt");

  for (bool change_term : {false, true}) {
    Fixture f;
    const auto retained = f.Issue();
    Check(bool(retained), "resolver-mutation fixture issuance failed");
    unsigned calls = 0;
    f.input.mga_authority.resolve_current = [&f, &calls, change_term] {
      if (++calls == 2) {
        if (change_term)
          f.term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
        else
          ++f.input.physical_dag.nodes[1].causal_counter_id;
      }
      exec::CanonicalMgaCurrentResolution current;
      current.statement_context = f.input.mga_authority.statement_context;
      return current;
    };
    actual = 99;
    Check(!f.Matches(*retained, &actual) && actual == 0,
          "final resolver mutation escaped retained binding comparison");
  }

  for (bool validate : {false, true}) {
    bool completed = false;
    for (long position = 0; position < 4096; ++position) {
      Fixture f;
      actual = 99;
      fault::remaining = position; fault::hit = false;
      auto issued = validate ? std::shared_ptr<const Receipt>{} : f.Issue();
      const bool matched = validate ? f.Matches(*receipt, &actual) : false;
      const bool hit = fault::hit;
      fault::remaining = -1;
      if (hit) {
        ++faults;
        Check(validate ? (!matched && actual == 0) : !issued,
              "allocation failure published receipt or successful match");
        Check(f.Matches(*receipt), "failed allocation corrupted existing receipt");
      } else {
        Check(validate ? matched : bool(issued), "uninjected operation failed");
        completed = true; break;
      }
    }
    Check(completed, "allocation fault enumeration never completed");
  }
}
}

int main() {
  try {
    Verify();
    VerifyNtileExecution();
    std::cout << "PASS checks=" << checks << " allocation_faults=" << faults << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    fault::remaining = -1;
    std::cerr << error.what() << '\n'; return EXIT_FAILURE;
  }
}
