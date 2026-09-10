// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_datatype_operator_authority_provider.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "../sbsql_sblr_alignment/typed_delete_test_fixture.hpp"
#include <functional>
#include <iostream>

namespace api = scratchbird::engine::internal_api;
namespace fixture = scratchbird::tests::delete_carrier;
namespace wire = scratchbird::wire;
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; fixture::Check(good, message); }
api::EngineRequestContext Context(const fixture::Fixture& f, std::string phase) {
  api::EngineRequestContext c;
  const auto text = api::datatype_operator_projection::UuidText;
  c.database_path = "/component/no_database_access";
  c.database_uuid.canonical = text(fixture::Uuid(80));
  c.session_uuid.canonical = text(fixture::Uuid(81));
  c.principal_uuid.canonical = text(fixture::Uuid(82));
  c.transaction_uuid.canonical = text(f.descriptor.owning_transaction_uuid);
  c.local_transaction_id = f.descriptor.owning_local_transaction_id;
  c.statement_receipt_uuid.canonical = text(f.descriptor.authenticated_statement_receipt_uuid);
  c.statement_snapshot_uuid.canonical = text(f.descriptor.statement_snapshot_uuid);
  c.statement_snapshot_generation = 1;
  c.statement_metadata_snapshot_uuid.canonical = text(f.descriptor.catalog_snapshot_uuid);
  c.catalog_generation_id = f.descriptor.catalog_generation;
  c.datatype_catalog_snapshot_uuid.canonical = text(wire::kTypedUpdateDatatypeSnapshotUuid);
  c.datatype_catalog_generation = c.datatype_registry_generation = 1;
  c.statement_metadata_snapshot_engine_owned = c.security_context_present = true;
  c.authorization_context.present = true;
  c.authorization_context.principal_uuid = c.principal_uuid;
  c.authorization_context.authority_uuid.canonical = text(f.descriptor.security_context_uuid);
  c.authorization_context.security_context_generation = f.descriptor.security_generation;
  c.trace_tags = {std::move(phase)};
  return c;
}
int main() try {
  for (unsigned profile = 0; profile != 3; ++profile) {
    auto f = fixture::Make(profile);
    auto binder = Context(f, "private_dml_delete_rows_binder");
    auto consumer = Context(f, "private_dml_delete_rows_consumer");
    auto recovery = Context(f, "private_dml_delete_rows_recovery");
    const auto capture = [&](const api::EngineRequestContext& context, const fixture::Fixture& value) {
      return api::CaptureDmlDeleteDatatypeAuthorityV1({context, value.descriptor.exact_bytes, value.predicate.exact_bytes});
    };
    const auto captured = capture(binder, f);
    if (!captured.ok) std::cerr << captured.diagnostic.code << ':' << captured.diagnostic.detail << '\n';
    Check(captured.ok && captured.handle.valid(), "DELETE live registry capture");
    Check(captured.datatypes.exact_bytes == f.datatypes.exact_bytes &&
          captured.operators.exact_bytes == f.operators.exact_bytes, "exact DELETE live registry projection");
    Check(!api::RevalidateDmlDeleteDatatypeAuthorityV1(consumer, captured).error, "DELETE consumer revalidation");
    Check(api::MatchesDmlDeleteDatatypeCaptureV1(captured, f.descriptor, f.predicate), "exact DELETE source capture");
    auto different_source = f;
    if (different_source.predicate.records.size() == 1)
      different_source.predicate.records.front().node_occurrence_uuid = fixture::Uuid(91);
    else
      different_source.predicate.records[1].canonical_value[0] ^= 1;
    fixture::Seal(different_source);
    Check(!api::MatchesDmlDeleteDatatypeCaptureV1(captured, different_source.descriptor, different_source.predicate),
          "valid same-owner different predicate cannot substitute captured source");
    Check(!api::RevalidateRecoveredDmlDeleteDatatypeAuthorityV1(recovery, f.descriptor, f.predicate,
          captured.datatypes, captured.operators).error, "DELETE recovered registry revalidation");
    Check(api::RevalidateDmlDeleteDatatypeAuthorityV1(binder, captured).error, "binder cannot consume");
    Check(!capture(consumer, f).ok && !capture(recovery, f).ok, "consumer/recovery cannot bind");
    for (const auto tag : {"private_dml_update_rows_binder", "private_dml_update_rows_consumer",
                           "private_dml_update_rows_recovery"}) {
      auto changed = binder; changed.trace_tags.push_back(tag);
      Check(!capture(changed, f).ok, "UPDATE phase cannot confer DELETE authority");
      changed.trace_tags = {tag};
      Check(!capture(changed, f).ok, "UPDATE phase cannot replace DELETE authority");
    }
    const std::vector<std::function<void(api::EngineRequestContext&)>> stale = {
      [](auto& c) { c.database_path += ".other"; },
      [](auto& c) { c.database_uuid = c.session_uuid; },
      [](auto& c) { c.session_uuid = c.database_uuid; },
      [](auto& c) { c.principal_uuid = c.session_uuid; c.authorization_context.principal_uuid = c.principal_uuid; },
      [](auto& c) { ++c.local_transaction_id; },
      [](auto& c) { c.transaction_uuid = c.session_uuid; },
      [](auto& c) { c.statement_receipt_uuid = c.session_uuid; },
      [](auto& c) { c.statement_snapshot_uuid = c.session_uuid; },
      [](auto& c) { ++c.statement_snapshot_generation; },
      [](auto& c) { c.statement_metadata_snapshot_uuid = c.session_uuid; },
      [](auto& c) { ++c.catalog_generation_id; },
      [](auto& c) { ++c.datatype_catalog_generation; },
      [](auto& c) { ++c.datatype_registry_generation; },
      [](auto& c) { c.authorization_context.authority_uuid = c.session_uuid; },
      [](auto& c) { ++c.authorization_context.security_context_generation; },
      [](auto& c) { ++c.security_epoch; },
      [](auto& c) { c.read_only_mode = true; },
      [](auto& c) { c.cluster_transaction_active = true; },
      [](auto& c) { c.route_fence_present = true; },
      [](auto& c) { c.statement_metadata_snapshot_engine_owned = false; },
      [](auto& c) { c.security_context_present = false; },
      [](auto& c) { c.authorization_context.present = false; },
    };
    for (const auto& mutate : stale) {
      auto changed = consumer; mutate(changed);
      Check(api::RevalidateDmlDeleteDatatypeAuthorityV1(changed, captured).error, "changed owner admitted");
    }
    auto changed = captured; ++changed.datatypes.records.front().codec_generation;
    Check(api::RevalidateDmlDeleteDatatypeAuthorityV1(consumer, changed).error, "mutated datatype projection admitted");
    Check(api::RevalidateRecoveredDmlDeleteDatatypeAuthorityV1(recovery, f.descriptor, f.predicate,
          changed.datatypes, changed.operators).error, "mutated recovered projection admitted");
    changed = captured; changed.datatypes.records.front().exact_bytes[16] ^= 1;
    Check(api::RevalidateDmlDeleteDatatypeAuthorityV1(consumer, changed).error, "mutated retained row admitted");
    changed = captured; changed.handle = {};
    Check(api::RevalidateDmlDeleteDatatypeAuthorityV1(consumer, changed).error, "bytes cannot manufacture handle");
    auto bad = f; bad.descriptor.owning_local_transaction_id++;
    fixture::Seal(bad);
    Check(!capture(binder, bad).ok, "cross-transaction descriptor admitted");
    bad = f; bad.descriptor.security_context_uuid = fixture::Uuid(90);
    fixture::Seal(bad);
    Check(!capture(binder, bad).ok, "cross-security descriptor admitted");
  }
  std::cout << "DELETE datatype authority checks=" << checks << " failures=0\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n'; return 1;
}
