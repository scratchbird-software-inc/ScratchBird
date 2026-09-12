// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "copy_on_write.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
using Phase = mga::CopyOnWriteMutationPhase;
using Kind = mga::CopyOnWriteMutationKind;

namespace {
std::size_t checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL check=" << checks << " " << message << '\n';
    std::exit(1);
  }
}

platform::TypedUuid Identity(platform::UuidKind kind, unsigned char suffix) {
  platform::TypedUuid result;
  result.kind = kind;
  result.value.bytes = {0x01,0x9a,0x08,0x13,0x41,0x6b,0x70,0x01,
                        0x80,0,0,0,0,0,0,suffix};
  return result;
}

mga::CopyOnWriteMutationState State(Kind kind, Phase phase, bool required, bool written) {
  mga::CopyOnWriteMutationState result;
  result.intent.kind = kind;
  result.intent.transaction.local_id.value = 7;
  result.intent.transaction.transaction_uuid = Identity(platform::UuidKind::transaction, 1);
  result.intent.transaction.scope = mga::TransactionScope::local_node;
  result.intent.row.row_uuid = Identity(platform::UuidKind::row, 2);
  result.intent.has_base_version = kind != Kind::insert;
  result.intent.base_version_sequence = kind == Kind::insert ? 0 : 12;
  result.intent.new_version_sequence = 13;
  result.intent.payload_required = kind != Kind::delete_row;
  result.intent.system_catalog_mutation = kind == Kind::system_catalog_update;
  result.resulting_row_state = kind == Kind::delete_row ? mga::RowVersionState::delete_marker
                                                      : mga::RowVersionState::uncommitted;
  result.phase = phase;
  result.evidence_record_required = required;
  result.evidence_record_written = written;
  return result;
}

bool Equal(const mga::CopyOnWriteMutationState& a, const mga::CopyOnWriteMutationState& b) {
  // Field-wise comparison: object padding is not part of the state contract.
  return a.intent.kind == b.intent.kind &&
      a.intent.transaction.local_id.value == b.intent.transaction.local_id.value &&
      a.intent.transaction.scope == b.intent.transaction.scope &&
      a.intent.transaction.transaction_uuid.kind == b.intent.transaction.transaction_uuid.kind &&
      a.intent.transaction.transaction_uuid.value.bytes == b.intent.transaction.transaction_uuid.value.bytes &&
      a.intent.row.row_uuid.kind == b.intent.row.row_uuid.kind &&
      a.intent.row.row_uuid.value.bytes == b.intent.row.row_uuid.value.bytes &&
      a.intent.base_version_sequence == b.intent.base_version_sequence &&
      a.intent.new_version_sequence == b.intent.new_version_sequence &&
      a.intent.has_base_version == b.intent.has_base_version &&
      a.intent.payload_required == b.intent.payload_required &&
      a.intent.system_catalog_mutation == b.intent.system_catalog_mutation &&
      a.phase == b.phase && a.resulting_row_state == b.resulting_row_state &&
      a.evidence_record_required == b.evidence_record_required &&
      a.evidence_record_written == b.evidence_record_written;
}

// Explicit transition compatibility oracle, independent of the production switch.
// This component does not classify transaction inventory finality or reclaim pages.
constexpr std::array<std::pair<Phase, Phase>, 17> edges{{
  {Phase::planned, Phase::base_version_locked},
  {Phase::planned, Phase::new_version_allocated},
  {Phase::planned, Phase::rollback_pending},
  {Phase::base_version_locked, Phase::new_version_allocated},
  {Phase::base_version_locked, Phase::rollback_pending},
  {Phase::base_version_locked, Phase::recovery_required},
  {Phase::new_version_allocated, Phase::payload_written_unpublished},
  {Phase::new_version_allocated, Phase::rollback_pending},
  {Phase::new_version_allocated, Phase::recovery_required},
  {Phase::payload_written_unpublished, Phase::publish_pending_transaction},
  {Phase::payload_written_unpublished, Phase::rollback_pending},
  {Phase::payload_written_unpublished, Phase::recovery_required},
  {Phase::publish_pending_transaction, Phase::published},
  {Phase::publish_pending_transaction, Phase::rollback_pending},
  {Phase::publish_pending_transaction, Phase::recovery_required},
  {Phase::rollback_pending, Phase::rollback_complete},
  {Phase::rollback_pending, Phase::recovery_required}
}};
constexpr std::array phases{Phase::planned, Phase::base_version_locked,
  Phase::new_version_allocated, Phase::payload_written_unpublished,
  Phase::publish_pending_transaction, Phase::published, Phase::rollback_pending,
  Phase::rollback_complete, Phase::recovery_required, Phase::unknown};
constexpr std::array kinds{Kind::insert, Kind::update, Kind::delete_row, Kind::system_catalog_update};
bool Edge(Phase from, Phase to) {
  if (from == Phase::unknown || to == Phase::unknown) return false;
  for (auto pair : edges) if (pair.first == from && pair.second == to) return true;
  return false;
}
}

int main() {
  constexpr auto expected = kinds.size() * phases.size() * phases.size() * 4;
  std::cout << "expected_transition_cases=" << expected << '\n' << std::flush;
  // Dedicated regression: validating only the old state must not authorize
  // publication of a candidate that lacks its required evidence.
  auto pending = State(Kind::insert, Phase::publish_pending_transaction, true, false);
  auto refused = mga::AdvanceCopyOnWriteMutationPhase(pending, Phase::published);
  Check(!refused.ok(), "publication without required evidence must fail");
  Check(Equal(refused.mutation, pending), "refused transition retains original state");
  Check(!refused.diagnostic.diagnostic_code.empty(), "refusal carries a diagnostic");
  pending.evidence_record_written = true;
  auto published = mga::AdvanceCopyOnWriteMutationPhase(pending, Phase::published);
  Check(published.ok() && published.mutation.phase == Phase::published,
        "publication with evidence remains admitted");

  std::size_t executed = 0;
  for (auto kind : kinds) for (auto from : phases) for (auto to : phases)
    for (bool required : {false, true}) for (bool written : {false, true}) {
      const auto input = State(kind, from, required, written);
      const auto saved = input;
      const bool missing = required && !written;
      const bool expected_ok = from != Phase::unknown &&
          !(from == Phase::published && missing) && Edge(from, to) &&
          !(to == Phase::published && missing);
      const auto result = mga::AdvanceCopyOnWriteMutationPhase(input, to);
      Check(result.ok() == expected_ok, "phase/kind/evidence outcome matches independent oracle");
      Check(Equal(input, saved), "caller state remains immutable");
      auto expected_state = input;
      if (expected_ok) expected_state.phase = to;
      Check(Equal(result.mutation, expected_state), "only admitted phase may change");
      if (expected_ok) {
        Check(mga::ValidateCopyOnWriteMutationState(result.mutation).ok(),
              "every successful transition yields a valid state");
      } else {
        Check(!result.diagnostic.diagnostic_code.empty(), "every refusal is diagnosed");
        Check(result.status.code == result.diagnostic.status.code &&
              result.status.severity == result.diagnostic.status.severity &&
              result.status.subsystem == result.diagnostic.status.subsystem,
              "result and diagnostic status agree");
      }
      ++executed;
    }
  Check(executed == expected, "expected and executed populations agree");
  std::cout << "PASS transition_cases=" << executed << " checks=" << checks << '\n';
}
