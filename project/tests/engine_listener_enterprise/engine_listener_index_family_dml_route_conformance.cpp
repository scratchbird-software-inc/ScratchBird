// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"
#include "index_family_registry.hpp"
#include "index_maintenance.hpp"
#include "index_route_capability.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "engine_listener_index_family_dml_route_conformance: "
            << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

bool AcceptedRuntimeFamily(const idx::IndexFamilyDescriptor& descriptor) {
  return descriptor.completion ==
             idx::IndexCompletionStatus::accepted_requires_full_implementation &&
         descriptor.persistence != idx::IndexPersistenceClass::donor_emulated &&
         descriptor.persistence != idx::IndexPersistenceClass::policy_blocked;
}

bool OrderedTreeFamily(idx::IndexFamily family) {
  return family == idx::IndexFamily::btree ||
         family == idx::IndexFamily::unique_btree ||
         family == idx::IndexFamily::expression ||
         family == idx::IndexFamily::partial ||
         family == idx::IndexFamily::covering;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view value) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(value) != std::string::npos;
                     });
}

const std::string& TableUuid() {
  static const std::string uuid =
      UuidText(platform::UuidKind::object, 1771300005000ull, 0x3f);
  return uuid;
}

api::CrudIndexRecord IndexFor(const idx::IndexFamilyDescriptor& descriptor,
                              const std::string& index_uuid) {
  api::CrudIndexRecord index;
  index.creator_tx = 700;
  index.index_uuid = index_uuid;
  index.table_uuid = TableUuid();
  index.family = descriptor.id;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.column_name = "name";
  if (descriptor.family == idx::IndexFamily::unique_btree) {
    index.unique = true;
    index.key_envelopes.push_back("unique");
  }
  if (descriptor.family == idx::IndexFamily::partial) {
    index.predicate_kind = "where_eq";
    index.predicate_column = "payload";
    index.predicate_value = "visible";
  }
  if (descriptor.family == idx::IndexFamily::covering) {
    index.include_columns.push_back("payload");
  }
  return index;
}

api::DmlIndexWriteRowImage Row(std::string row_uuid,
                               std::string version_uuid,
                               std::string name,
                               std::string payload = "visible") {
  api::DmlIndexWriteRowImage row;
  row.row_uuid = std::move(row_uuid);
  row.version_uuid = std::move(version_uuid);
  row.values.push_back({"id", "1"});
  row.values.push_back({"name", std::move(name)});
  row.values.push_back({"payload", std::move(payload)});
  return row;
}

api::DmlIndexWriteEvent BaseEvent(api::DmlIndexWriteOperation operation,
                                  const api::CrudIndexRecord& index,
                                  platform::u64 salt) {
  api::DmlIndexWriteEvent event;
  event.operation = operation;
  event.index = index;
  event.table_uuid = TableUuid();
  event.transaction_uuid = UuidText(platform::UuidKind::transaction,
                                    1771300000000ull + salt,
                                    0x31);
  event.local_transaction_id = 700 + salt;
  event.mga_transaction_identity_proof = true;
  event.mga_transaction_finality_authority_proof = true;
  event.rollback_evidence_token = "rollback-token-eler040";
  event.index_descriptor_capability_proof = true;
  event.key_extraction_proof = true;
  event.partial_predicate_proof = true;
  event.covering_payload_proof = true;
  event.unique_preflight_proof = true;
  event.unique_reservation_preflight_proof = true;
  return event;
}

page::IndexBtreePhysicalTree MakeTree(const std::string& index_uuid) {
  const auto parsed =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(parsed.ok(), "index uuid parse failed");
  auto initialized = page::InitializeIndexBtreePhysicalTree(parsed.value, 4096);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

api::DmlIndexWritePathRequest RequestFor(
    std::vector<api::DmlIndexWriteEvent> events,
    page::IndexBtreePhysicalTree* tree,
    idx::PersistentSecondaryIndexDeltaLedger* ledger) {
  Require(!events.empty(), "DML proof request requires events");
  api::DmlIndexWritePathRequest request;
  request.events = std::move(events);
  if (tree != nullptr) {
    request.physical_trees.push_back(
        {request.events.front().index.index_uuid, tree});
  }
  if (ledger != nullptr) {
    request.secondary_delta_ledgers.push_back(
        {request.events.front().index.index_uuid, ledger, {}});
  }
  if (ledger != nullptr) {
    request.deferred_secondary_index_options = {
        idx::kDeferredSecondaryIndexRuntimeOption,
        idx::kSecondaryIndexDeltaLedgerFeatureOption,
        idx::kDeltaLedgerReaderOverlayOption,
        idx::kDeltaLedgerCleanupHorizonBoundOption,
        idx::kDeltaLedgerRecoveryClassifiableOption};
    request.cleanup_horizon_token = "cleanup-horizon:eler040";
    request.durable_mga_inventory_proof = true;
    request.delta_overlay_read_proof = true;
    request.recovery_classification_proof = true;
    request.unique_reservation_protocol_proof = true;
    request.unique_deferred_route_closure_proof = true;
  }
  return request;
}

void RequireMaintenanceProof(const idx::IndexFamilyDescriptor& descriptor,
                             const platform::TypedUuid& index_uuid) {
  idx::IndexMaintenanceRequest verify;
  verify.index_uuid = index_uuid;
  verify.family = descriptor.family;
  verify.operation = idx::IndexMaintenanceOperation::verify;
  verify.page_budget = 1;
  const auto verified = idx::PlanIndexMaintenance(verify);
  Require(verified.ok(), "verify maintenance route refused accepted family");

  idx::IndexMaintenanceRequest rebuild = verify;
  rebuild.operation = idx::IndexMaintenanceOperation::rebuild;
  rebuild.policy_allows_mutation = true;
  const auto rebuilt = idx::PlanIndexMaintenance(rebuild);
  Require(rebuilt.ok(), "rebuild maintenance route refused accepted family");
}

void ProveOrderedFamily(const idx::IndexFamilyDescriptor& descriptor,
                        const std::string& index_uuid,
                        const std::string& row_uuid,
                        const std::string& v1,
                        const std::string& v2) {
  auto tree = MakeTree(index_uuid);
  const auto index = IndexFor(descriptor, index_uuid);

  auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, index, 1);
  insert.has_new_row = true;
  insert.new_row = Row(row_uuid, v1, "alpha");
  auto result = api::ApplyDmlIndexWritePath(RequestFor({insert}, &tree, nullptr));
  Require(result.ok && result.physical_inserts >= 1,
          "ordered family insert did not mutate physical tree");
  Require(HasEvidence(result.evidence, "dml_index_route_capability", "complete"),
          "ordered family route evidence missing");

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, 2);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "bravo");
  idx::PersistentSecondaryIndexDeltaLedger ordered_ledger;
  result = api::ApplyDmlIndexWritePath(
      RequestFor({update}, &tree, &ordered_ledger));
  Require(result.ok && result.physical_deletes >= 1 &&
              result.physical_inserts >= 1,
          "ordered family update did not rewrite physical tree");
  Require(ordered_ledger.records.empty(),
          "ordered family update incorrectly selected deferred ledger route");

  auto delete_row = BaseEvent(api::DmlIndexWriteOperation::delete_row, index, 3);
  delete_row.has_old_row = true;
  delete_row.old_row = Row(row_uuid, v2, "bravo");
  result = api::ApplyDmlIndexWritePath(RequestFor({delete_row}, &tree, nullptr));
  Require(result.ok && result.physical_deletes >= 1,
          "ordered family delete did not mutate physical tree");
}

void ProveLedgerFamily(const idx::IndexFamilyDescriptor& descriptor,
                       const std::string& index_uuid,
                       const std::string& row_uuid,
                       const std::string& v1,
                       const std::string& v2) {
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  const auto index = IndexFor(descriptor, index_uuid);

  auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, index, 11);
  insert.has_new_row = true;
  insert.new_row = Row(row_uuid, v1, "alpha");
  auto result =
      api::ApplyDmlIndexWritePath(RequestFor({insert}, nullptr, &ledger));
  Require(result.ok && result.secondary_delta_ledger_appends >= 1,
          "ledger family insert did not append mutation record");

  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, 12);
  update.has_old_row = true;
  update.old_row = Row(row_uuid, v1, "alpha");
  update.has_new_row = true;
  update.new_row = Row(row_uuid, v2, "bravo");
  result = api::ApplyDmlIndexWritePath(RequestFor({update}, nullptr, &ledger));
  Require(result.ok && result.secondary_delta_ledger_appends >= 2 &&
              result.secondary_delta_overlay_reads == 1,
          "ledger family update did not append before/after overlay records");

  auto delete_row = BaseEvent(api::DmlIndexWriteOperation::delete_row, index, 13);
  delete_row.has_old_row = true;
  delete_row.old_row = Row(row_uuid, v2, "bravo");
  result =
      api::ApplyDmlIndexWritePath(RequestFor({delete_row}, nullptr, &ledger));
  Require(result.ok && result.secondary_delta_ledger_appends >= 1,
          "ledger family delete did not append mutation record");
  Require(ledger.records.size() >= 4,
          "ledger family did not retain all mutation records");

  const auto encoded =
      idx::EncodePersistentSecondaryIndexDeltaLedger(ledger, {});
  Require(encoded.ok(), "ledger family encode failed");
  const auto decoded =
      idx::DecodePersistentSecondaryIndexDeltaLedger(encoded.bytes, {});
  Require(decoded.ok() &&
              idx::PersistentSecondaryIndexDeltaLedgerEquals(ledger,
                                                             decoded.ledger),
          "ledger family reopen decode changed mutation records");
  const auto recovery =
      idx::ClassifySecondaryIndexDeltaLedgerForRecovery(decoded.ledger);
  Require(recovery.ok() &&
              recovery.action ==
                  idx::SecondaryIndexDeltaLedgerRecoveryAction::
                      retain_for_mga_transaction_finality,
          "ledger family recovery classification did not preserve MGA authority");
}

std::string Csv(std::string_view value) {
  if (value.find_first_of(",\"\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') {
      out += "\"\"";
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<std::vector<std::string>>& rows) {
  if (!path.empty()) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(out), "could not open ELER-040 matrix");
    out << "family_id,status,dml_insert,dml_update,dml_delete,mutation_path\n";
    for (const auto& row : rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) { out << ','; }
        out << Csv(row[i]);
      }
      out << '\n';
    }
    out.close();
    Require(static_cast<bool>(out), "could not write ELER-040 matrix");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::vector<std::string>> matrix;
  std::size_t accepted = 0;
  std::size_t ordered = 0;
  std::size_t ledger = 0;
  std::size_t refused = 0;

  platform::byte suffix = 0x40;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const bool runtime = AcceptedRuntimeFamily(descriptor);
    for (const auto route : {idx::IndexRouteKind::dml_insert,
                             idx::IndexRouteKind::dml_update,
                             idx::IndexRouteKind::dml_delete}) {
      const auto* state =
          idx::FindBuiltinIndexRouteCapabilityState(route, descriptor.family);
      Require(state != nullptr, "DML route capability state missing");
      if (runtime) {
        Require(state->route_complete() && state->supports_write &&
                    state->supports_mutation,
                "accepted index family missing DML route support");
      } else {
        Require(!state->route_complete(),
                "non-runtime index family was admitted to DML route");
      }
    }

    const std::string index_uuid =
        UuidText(platform::UuidKind::object, 1771300010000ull + suffix, suffix);
    const auto typed_index =
        uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                             index_uuid);
    Require(typed_index.ok(), "typed index uuid parse failed");
    const std::string row_uuid =
        UuidText(platform::UuidKind::row, 1771300020000ull + suffix, suffix);
    const std::string v1 =
        UuidText(platform::UuidKind::row, 1771300030000ull + suffix, suffix);
    const std::string v2 =
        UuidText(platform::UuidKind::row, 1771300040000ull + suffix, suffix);

    std::string path = "refused";
    if (runtime) {
      ++accepted;
      RequireMaintenanceProof(descriptor, typed_index.value);
      if (OrderedTreeFamily(descriptor.family)) {
        ++ordered;
        path = "physical_btree_tree";
        ProveOrderedFamily(descriptor, index_uuid, row_uuid, v1, v2);
      } else {
        ++ledger;
        path = "persistent_secondary_delta_ledger";
        ProveLedgerFamily(descriptor, index_uuid, row_uuid, v1, v2);
      }
    } else {
      ++refused;
      const auto index = IndexFor(descriptor, index_uuid);
      auto insert = BaseEvent(api::DmlIndexWriteOperation::insert, index, 21);
      insert.has_new_row = true;
      insert.new_row = Row(row_uuid, v1, "alpha");
      idx::PersistentSecondaryIndexDeltaLedger refused_ledger;
      const auto result = api::ApplyDmlIndexWritePath(
          RequestFor({insert}, nullptr, &refused_ledger));
      Require(!result.ok, "non-runtime family DML mutation was admitted");
    }

    matrix.push_back({descriptor.id,
                      runtime ? "accepted" : "refused",
                      runtime ? "complete" : "fail_closed",
                      runtime ? "complete" : "fail_closed",
                      runtime ? "complete" : "fail_closed",
                      path});
    ++suffix;
  }

  Require(accepted != 0 && ordered != 0 && ledger != 0 && refused != 0,
          "ELER-040 matrix did not exercise all route classes");
  if (argc == 2) { WriteMatrix(argv[1], matrix); }
  std::cout << "engine_listener_index_family_dml_route_conformance=passed"
            << " accepted=" << accepted
            << " ordered=" << ordered
            << " ledger=" << ledger
            << " refused=" << refused << '\n';
  return EXIT_SUCCESS;
}
