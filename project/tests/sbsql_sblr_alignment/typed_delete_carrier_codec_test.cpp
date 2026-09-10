// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "typed_delete_carrier_codec.hpp"
#include "typed_delete_test_fixture.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace w = scratchbird::wire;
namespace {
unsigned checks = 0;
void Require(bool good, const char* message) {
  ++checks;
  if (!good) { std::cerr << message << '\n'; std::exit(1); }
}
w::TypedUpdateUuid Uuid(unsigned value) {
  w::TypedUpdateUuid id{}; id[0] = 1; id[6] = 0x70; id[8] = 0x80;
  id[15] = static_cast<w::byte>(value); return id;
}
w::TypedDeleteDescriptorCarrier Descriptor() {
  w::TypedDeleteDescriptorCarrier v;
  v.descriptor_uuid = Uuid(1);
  v.authenticated_statement_receipt_uuid = Uuid(2);
  v.operation_uuid = Uuid(3);
  v.owning_transaction_uuid = Uuid(4);
  v.statement_snapshot_uuid = Uuid(5);
  v.catalog_snapshot_uuid = Uuid(6);
  v.security_context_uuid = Uuid(7);
  v.security_snapshot_uuid = Uuid(8);
  v.target_relation_uuid = Uuid(9);
  v.target_relation_occurrence_uuid = Uuid(10);
  v.predicate_expression_uuid = Uuid(11);
  v.row_policy_set_uuid = Uuid(12);
  v.constraint_set_uuid = Uuid(13);
  v.trigger_set_uuid = Uuid(14);
  v.deterministic_target_order_uuid = Uuid(15);
  v.resource_budget_uuid = Uuid(16);
  v.recovery_token_uuid = Uuid(17);
  v.builtin_operator_snapshot_uuid = Uuid(18);
  v.descriptor_generation = 1;
  v.structural_occurrence_id = 1;
  v.operation_generation = 1;
  v.owning_local_transaction_id = 1;
  v.catalog_generation = 1;
  v.datatype_registry_generation = 1;
  v.security_generation = 1;
  v.target_relation_generation = 1;
  v.target_relation_occurrence_generation = 1;
  v.predicate_expression_generation = 1;
  v.predicate_root_node_id = 1;
  v.row_policy_set_generation = 1;
  v.constraint_set_generation = 1;
  v.trigger_set_generation = 1;
  v.deterministic_target_order_generation = 1;
  v.resource_budget_generation = 1;
  v.recovery_generation = 1;
  v.executor_availability_generation = 1;
  v.builtin_operator_registry_generation = 1;
  v.predicate_vector_sha256.fill(1);
  v.row_policy_set_sha256.fill(2);
  v.ordered_constraint_set_sha256.fill(3);
  v.ordered_trigger_set_sha256.fill(4);
  v.predicate_node_count = 1;
  v.builtin_operator_snapshot_uuid = w::kTypedUpdateOperatorSnapshotUuid;
  return v;
}
void Rehash(std::vector<w::byte>& bytes, std::size_t offset, std::string_view domain,
            std::size_t suffix = 0) {
  std::vector<w::byte> material(domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.begin() + offset);
  if (suffix) material.insert(material.end(), bytes.begin() + suffix, bytes.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  Require(digest.ok(), "hash failure");
  std::copy(digest.digest.begin(), digest.digest.end(), bytes.begin() + offset);
}
template<class Decode> void CorruptionMatrix(const std::vector<w::byte>& good, Decode decode) {
  for (std::size_t n = 0; n < good.size(); ++n)
    Require(!decode(std::span<const w::byte>(good).first(n)), "truncation admitted");
  for (std::size_t n = 0; n < good.size(); ++n) {
    auto bad = good; bad[n] ^= 1;
    Require(!decode(bad), "single-byte corruption admitted");
  }
  auto trailing = good; trailing.push_back(0);
  Require(!decode(trailing), "trailing byte admitted");
}
w::TypedDeleteJournalRecord Roundtrip(w::TypedDeleteJournalRecord value,
                                     const w::TypedDeleteJournalRecord* previous) {
  w::TypedDeleteCarrierError error;
  std::vector<w::byte> encoded, again;
  Require(w::EncodeTypedDeleteJournal(value, previous, &encoded, &error), error.field.c_str());
  w::TypedDeleteJournalRecord decoded;
  Require(w::DecodeAndValidateTypedDeleteJournal(encoded, previous, &decoded, &error), error.field.c_str());
  Require(w::EncodeTypedDeleteJournal(decoded, previous, &again, &error) && encoded == again,
          "journal roundtrip differs");
  CorruptionMatrix(encoded, [&](auto bytes) {
    w::TypedDeleteJournalRecord candidate;
    return w::DecodeAndValidateTypedDeleteJournal(bytes, previous, &candidate, &error);
  });
  return decoded;
}
}
int main() try {
  for (unsigned profile = 0; profile != 3; ++profile) {
    namespace fixture = scratchbird::tests::delete_carrier;
    const auto f = fixture::Make(profile);
    const auto valid = [](const fixture::Fixture& c) {
      w::TypedDeleteCarrierError error;
      return w::ValidateTypedDeleteDatatypeOperatorAuthority(
          c.descriptor, c.predicate, c.datatypes, c.operators, &error);
    };
    Require(valid(f), "DELETE datatype/operator binding refused");
    for (unsigned field = 0; field != 7; ++field) {
      auto altered = f;
      auto* hash = field == 0 ? &altered.descriptor.descriptor_evidence_sha256 :
                   field == 1 ? &altered.predicate.identity.vector_sha256 :
                   field == 2 ? &altered.datatypes.identity.vector_sha256 :
                   field == 3 ? &altered.operators.identity.vector_sha256 :
                   field == 4 ? &altered.predicate.records.front().canonical_value_sha256 :
                   field == 5 ? &altered.predicate.records.front().node_evidence_sha256 :
                                &altered.datatypes.records.front().record_evidence_sha256;
      (*hash)[0] ^= 1;
      Require(!valid(altered), "altered decoded evidence hash admitted");
    }
    auto bad = f; ++bad.descriptor.descriptor_generation;
    Require(!valid(bad), "mutated DELETE descriptor projection admitted");
    bad = f; ++bad.predicate.records.front().output_codec_generation;
    Require(!valid(bad), "mutated predicate projection admitted");
    bad = f; ++bad.datatypes.records.front().codec_generation;
    Require(!valid(bad), "mutated datatype projection admitted");
    bad = f; bad.datatypes.records.front().exact_bytes[16] ^= 1;
    Require(!valid(bad), "altered retained datatype row admitted");
    bad = f; bad.datatypes.format_version = 2;
    Require(!valid(bad), "UPDATE TEXT format admitted to DELETE");
    bad = f; bad.predicate.identity.owner_descriptor_uuid = fixture::Uuid(90);
    fixture::Seal(bad);
    Require(!valid(bad), "rehashed cross-owner predicate admitted");
    bad = f; bad.datatypes.identity.owner_descriptor_uuid = fixture::Uuid(90);
    fixture::Seal(bad);
    Require(!valid(bad), "rehashed cross-owner datatype admitted");
    bad = f; bad.operators.identity.owner_descriptor_uuid = fixture::Uuid(90);
    fixture::Seal(bad);
    Require(!valid(bad), "rehashed cross-owner operator admitted");
    if (profile) {
      bad = f; bad.predicate.records.front().referenced_relation_occurrence_uuid = fixture::Uuid(90);
      fixture::Seal(bad);
      Require(!valid(bad), "cross-relation predicate admitted");
      bad = f; ++bad.operators.records.front().operator_generation;
      Require(!valid(bad), "altered operator projection admitted");
      bad = f; bad.operators.records.front().exact_bytes[16] ^= 1;
      Require(!valid(bad), "altered retained operator row admitted");
      bad = f; bad.predicate.records[1].canonical_value.pop_back();
      w::TypedUpdateCarrierError shared_error;
      std::vector<w::byte> bytes;
      if (w::EncodeTypedUpdatePredicateVector(bad.predicate, &bytes, &shared_error)) {
        fixture::Seal(bad);
        Require(!valid(bad), "short canonical literal admitted");
      } else Require(!shared_error.diagnostic_code.empty(), "short literal refusal has no diagnostic");
    } else {
      bad = f; bad.predicate.records[0].canonical_value = {0};
      w::TypedUpdateCarrierError shared_error;
      std::vector<w::byte> bytes;
      if (w::EncodeTypedUpdatePredicateVector(bad.predicate, &bytes, &shared_error)) {
        fixture::Seal(bad);
        Require(!valid(bad), "non-TRUE unfiltered predicate admitted");
      } else Require(!shared_error.diagnostic_code.empty(), "non-TRUE refusal has no diagnostic");
    }
  }
  w::TypedDeleteCarrierError error;
  auto descriptor = Descriptor();
  std::vector<w::byte> encoded, again;
  Require(w::EncodeTypedDeleteDescriptor(descriptor, &encoded, &error), "descriptor encode");
  Require(encoded.size() == 648, "descriptor width");
  for (auto [offset, id] : std::initializer_list<std::pair<unsigned, w::TypedUpdateUuid>>{
      {16, descriptor.descriptor_uuid},
      {40, descriptor.authenticated_statement_receipt_uuid},
      {64, descriptor.operation_uuid},
      {88, descriptor.owning_transaction_uuid},
      {112, descriptor.statement_snapshot_uuid},
      {128, descriptor.catalog_snapshot_uuid},
      {160, descriptor.security_context_uuid},
      {176, descriptor.security_snapshot_uuid},
      {200, descriptor.target_relation_uuid},
      {224, descriptor.target_relation_occurrence_uuid},
      {248, descriptor.predicate_expression_uuid},
      {320, descriptor.row_policy_set_uuid},
      {384, descriptor.constraint_set_uuid},
      {448, descriptor.trigger_set_uuid},
      {512, descriptor.deterministic_target_order_uuid},
      {536, descriptor.resource_budget_uuid},
      {560, descriptor.recovery_token_uuid},
      {592, descriptor.builtin_operator_snapshot_uuid}
  }) Require(std::equal(id.begin(), id.end(), encoded.begin() + offset), "binary16 UUID offset");
  w::TypedDeleteDescriptorCarrier decoded;
  Require(w::DecodeAndValidateTypedDeleteDescriptor(encoded, &decoded, &error), "descriptor decode");
  Require(w::EncodeTypedDeleteDescriptor(decoded, &again, &error) && again == encoded, "descriptor roundtrip");
  CorruptionMatrix(encoded, [&](auto bytes) { return w::DecodeAndValidateTypedDeleteDescriptor(bytes, &decoded, &error); });
  for (auto offset : {16, 40, 64, 88, 112, 128, 160, 176, 200, 224, 248, 320, 384, 448, 512, 536, 560, 592}) {
    auto bad = encoded; std::fill_n(bad.begin() + offset, 16, 0);
    Rehash(bad, 616, "ScratchBird.SblrDmlDeleteRowsDescriptor.V1");
    Require(!w::DecodeAndValidateTypedDeleteDescriptor(bad, &decoded, &error), "nil identity with valid hash");
  }
  for (auto offset : {32, 56, 80, 104, 144, 152, 192, 216, 240, 264, 272, 336, 400, 464, 528, 552, 576, 584, 608}) {
    auto bad = encoded; std::fill_n(bad.begin() + offset, 8, 0);
    Rehash(bad, 616, "ScratchBird.SblrDmlDeleteRowsDescriptor.V1");
    Require(!w::DecodeAndValidateTypedDeleteDescriptor(bad, &decoded, &error), "zero generation with valid hash");
  }
  for (auto offset : {284, 348, 412, 476}) {
    auto bad = encoded; bad[offset] = 1;
    Rehash(bad, 616, "ScratchBird.SblrDmlDeleteRowsDescriptor.V1");
    Require(!w::DecodeAndValidateTypedDeleteDescriptor(bad, &decoded, &error), "reserved byte with valid hash");
  }
  auto bad = descriptor; bad.row_policy_count = 2;
  Require(!w::EncodeTypedDeleteDescriptor(bad, &again, &error), "UPDATE USING/WITH CHECK pair admitted");
  bad = descriptor; bad.predicate_node_count = 3;
  Require(!w::EncodeTypedDeleteDescriptor(bad, &again, &error), "wrong predicate root admitted");
  bad.predicate_root_node_id = 3;
  Require(w::EncodeTypedDeleteDescriptor(bad, &again, &error), "equality shape refused");
  w::TypedUpdateDescriptorCarrier update_descriptor;
  w::TypedUpdateCarrierError update_error;
  Require(!w::DecodeAndValidateTypedUpdateDescriptor(encoded, &update_descriptor, &update_error), "DELETE decoded as UPDATE");
  auto substituted = encoded; substituted[1] = 'U';
  Rehash(substituted, 616, "ScratchBird.SblrDmlDeleteRowsDescriptor.V1");
  Require(!w::DecodeAndValidateTypedDeleteDescriptor(substituted, &decoded, &error), "UPDATE magic admitted");
  substituted = encoded;
  Rehash(substituted, 616, "ScratchBird.SblrDmlUpdateRowsDescriptor.V1");
  Require(!w::DecodeAndValidateTypedDeleteDescriptor(substituted, &decoded, &error), "UPDATE hash domain admitted");
  Require(!w::DecodeAndValidateTypedDeleteDescriptor(std::vector<w::byte>(424), &decoded, &error), "DELO extent admitted");

  w::TypedDeleteResultCarrier result;
  result.delete_descriptor_uuid = descriptor.descriptor_uuid;
  result.delete_descriptor_generation = descriptor.descriptor_generation;
  result.operation_uuid = descriptor.operation_uuid;
  result.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id = descriptor.owning_local_transaction_id;
  result.relation_uuid = descriptor.target_relation_uuid;
  result.relation_generation = descriptor.target_relation_generation;
  result.publication_barrier_uuid = Uuid(44); result.publication_barrier_generation = 1;
  result.effect_set_sha256.fill(1); result.executor_evidence_sha256.fill(2);
  result.matched_count = result.deleted_count = 2;
  Require(w::EncodeTypedDeleteResult(result, &encoded, &error), "result encode");
  w::TypedDeleteResultCarrier decoded_result;
  Require(w::DecodeAndValidateTypedDeleteResult(encoded, &decoded_result, &error), "result decode");
  CorruptionMatrix(encoded, [&](auto bytes) { return w::DecodeAndValidateTypedDeleteResult(bytes, &decoded_result, &error); });
  w::TypedUpdateResultCarrier update_result;
  Require(!w::DecodeAndValidateTypedUpdateResult(encoded, &update_result, &update_error), "DELETE result decoded as UPDATE");
  result.deleted_count = 1;
  Require(!w::EncodeTypedDeleteResult(result, &again, &error), "mismatched deleted count admitted");
  result.deleted_count = 2;
  result.deleted_count = result.matched_count = 0;
  Require(w::EncodeTypedDeleteResult(result, &again, &error), "zero effect result refused");
  result.deleted_count = result.matched_count = 2;

  w::TypedDeleteJournalRecord journal;
  journal.database_uuid = Uuid(50); journal.authenticated_statement_receipt_uuid = descriptor.authenticated_statement_receipt_uuid;
  journal.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  journal.owning_local_transaction_id = descriptor.owning_local_transaction_id;
  journal.operation_uuid = descriptor.operation_uuid; journal.recovery_token_uuid = descriptor.recovery_token_uuid;
  journal.recovery_generation = descriptor.recovery_generation; journal.descriptor = descriptor;
  journal.journal_sequence = 1;
  const auto bound = Roundtrip(journal, nullptr);
  journal = bound; journal.journal_sequence = 2; journal.prior_record_sha256 = bound.record_evidence_sha256;
  journal.lifecycle_state = w::TypedUpdateJournalState::aborted;
  const auto bound_abort = Roundtrip(journal, &bound);
  journal.lifecycle_state = w::TypedUpdateJournalState::intent;
  journal.statement_savepoint_uuid = Uuid(55); journal.statement_savepoint_generation = 1;
  const auto intent = Roundtrip(journal, &bound);
  journal = intent; journal.journal_sequence = 3; journal.prior_record_sha256 = intent.record_evidence_sha256;
  journal.lifecycle_state = w::TypedUpdateJournalState::aborted;
  const auto intent_abort = Roundtrip(journal, &intent);
  result.publication_barrier_uuid = intent.statement_savepoint_uuid;
  result.publication_barrier_generation = intent.statement_savepoint_generation;
  journal.lifecycle_state = w::TypedUpdateJournalState::prepared; journal.prior_result = result;
  const auto prepared = Roundtrip(journal, &intent);
  journal = prepared; journal.journal_sequence = 4; journal.prior_record_sha256 = prepared.record_evidence_sha256;
  journal.lifecycle_state = w::TypedUpdateJournalState::published;
  const auto published = Roundtrip(journal, &prepared);
  journal.lifecycle_state = w::TypedUpdateJournalState::aborted; journal.prior_result.reset();
  const auto prepared_abort = Roundtrip(journal, &prepared);
  for (const auto* terminal : {&published, &bound_abort, &intent_abort, &prepared_abort}) {
    auto next = *terminal; ++next.journal_sequence; next.prior_record_sha256 = terminal->record_evidence_sha256;
    Require(!w::EncodeTypedDeleteJournal(next, terminal, &again, &error), "terminal successor admitted");
  }
  auto changed = published; changed.prior_result->matched_count = changed.prior_result->deleted_count = 3;
  Require(!w::EncodeTypedDeleteJournal(changed, &prepared, &again, &error), "prepared result changed");
  changed = intent; changed.database_uuid = Uuid(99);
  Require(!w::EncodeTypedDeleteJournal(changed, &bound, &again, &error), "cross database successor");
  changed = intent; changed.descriptor.target_relation_generation++;
  Require(!w::EncodeTypedDeleteJournal(changed, &bound, &again, &error), "descriptor successor changed");
  auto forged_previous = prepared; ++forged_previous.journal_sequence;
  Require(!w::EncodeTypedDeleteJournal(published, &forged_previous, &again, &error), "mutated predecessor object");
  changed = published; ++changed.prior_result->relation_generation;
  Require(!w::EncodeTypedDeleteJournal(changed, &prepared, &again, &error), "result owner changed");
  changed = prepared; changed.statement_savepoint_uuid = Uuid(66);
  Require(!w::EncodeTypedDeleteJournal(changed, &intent, &again, &error), "savepoint successor changed");
  std::cout << "typed DELETE carrier checks=" << checks << " failures=0\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
