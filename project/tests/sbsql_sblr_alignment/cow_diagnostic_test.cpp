// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "copy_on_write.hpp"
#include "canonical_diagnostic_catalog.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace mga = scratchbird::transaction::mga;
namespace p = scratchbird::core::platform;
namespace diag = scratchbird::core::diagnostics;
using Kind = mga::CopyOnWriteMutationKind;
using Phase = mga::CopyOnWriteMutationPhase;
using RowState = mga::RowVersionState;
using TxState = mga::TransactionState;
namespace {
std::size_t checks = 0, failures = 0;
void Check(bool result, std::string_view message) {
  ++checks;
  if (!result && ++failures <= 12) std::cerr << "FAIL check=" << checks << ' ' << message << '\n';
}
p::TypedUuid Identity(p::UuidKind kind, unsigned char suffix) {
  p::TypedUuid result;
  result.kind = kind;
  result.value.bytes = {1,0x9a,8,0x13,0x41,0x6b,0x70,1,0x80,0,0,0,0,0,0,suffix};
  return result;
}
mga::CopyOnWriteMutationState Valid() {
  mga::CopyOnWriteMutationState s;
  s.intent.kind = Kind::update;
  s.intent.transaction = {{7},Identity(p::UuidKind::transaction,1),mga::TransactionScope::local_node};
  s.intent.row.row_uuid = Identity(p::UuidKind::row,2);
  s.intent.has_base_version = true;
  s.intent.base_version_sequence = 1;
  s.intent.new_version_sequence = 2;
  s.phase = Phase::planned;
  s.resulting_row_state = RowState::uncommitted;
  return s;
}
void Failure(p::Status status, const p::DiagnosticRecord& d, std::string_view code, unsigned number) {
  Check(!status.ok() && static_cast<unsigned>(status.code) == number, "exact native operation status");
  Check(std::string_view(p::StatusCodeName(status.code)).starts_with("mga_cow_"), "native status has owning name");
  Check(status.severity == p::Severity::error && status.subsystem == p::Subsystem::transaction_mga,
        "failure severity and subsystem");
  Check(d.diagnostic_code == code, code);
  Check(d.status.code == status.code && d.status.severity == status.severity &&
        d.status.subsystem == status.subsystem, "result and diagnostic agree");
  const auto* definition = diag::FindCanonicalDiagnosticCode(code);
  Check(definition && definition->is_failure && definition->severity == diag::CanonicalSeverity::error,
        "registered canonical failure");
  if (definition) {
    Check(definition->numeric_binding == std::to_string(number), "native registry binding agrees");
    Check(definition->sqlstate == (code == "MGA.COW.TRANSACTION_NOT_WRITABLE" ? "25000" :
          code == "MGA.COW.READ_ONLY_TRANSACTION" ? "25006" :
          code == "MGA.COW.EVIDENCE_REQUIRED" ? "55000" : "22023"), "exact SQLSTATE rendering metadata");
    Check(definition->required_outcome == "reject_without_mutation_preserve_inventory", "inventory outcome unchanged");
    Check(definition->retry_class == (code == "MGA.COW.EVIDENCE_REQUIRED"
          ? "retry_same_boundary_after_required_evidence" : "never_retry_without_input_or_authority_change"),
          "exact registered retry boundary");
  }
  Check(d.source_component == "transaction.mga.copy_on_write", "owning diagnostic source");
}
void Failure(const mga::CopyOnWriteMutationResult& r, std::string_view code, unsigned number) {
  Failure(r.status,r.diagnostic,code,number);
}
void Assessment(const mga::CleanupEligibilityResult& result, std::string_view code) {
  Check(result.ok() && result.status.severity == p::Severity::info, "assessment computed without operation failure");
  Check(result.status.subsystem == p::Subsystem::transaction_mga, "cleanup assessment subsystem");
  Check(result.decision != mga::CleanupEligibilityDecision::eligible_authoritative, "assessment never authorizes reclaim");
  Check(result.diagnostic.diagnostic_code == code, "canonical assessment diagnostic");
  Check(result.diagnostic.status.code == result.status.code &&
        result.diagnostic.status.severity == result.status.severity, "assessment statuses agree");
  const auto* d = diag::FindCanonicalDiagnosticCode(code);
  Check(d && !d->is_failure && d->severity == diag::CanonicalSeverity::informational, "registered nonfailure");
  if (d) Check(d->required_outcome == "retain_versions_require_cleanup_authority" &&
      d->retry_class == "reassess_after_authority_or_horizon_change" &&
      d->numeric_binding == "not_applicable", "non-authoritative assessment contract");
  for (const auto& arg : result.diagnostic.arguments)
    Check(arg.value.find("PRIVATE-HORIZON-LABEL") == std::string::npos, "untrusted label is not diagnostic detail");
}
}
int main() {
  // Expected populations are fixed before invoking production code. These are
  // native helper tests, not a substitute for ordinary SQL/IPC execution.
  std::cout << "expected_enum_cases=262144 expected_named_diagnostics=17\n" << std::flush;
  auto s = Valid();
  Check(mga::ValidateCopyOnWriteMutationState(s).ok(), "valid descriptor positive");
  const auto check = [](mga::CopyOnWriteMutationState value, std::string_view code, unsigned n) {
    const auto r = mga::ValidateCopyOnWriteMutationState(value);
    Failure(r,code,n);
    Check(r.mutation.phase == value.phase && r.mutation.intent.new_version_sequence == value.intent.new_version_sequence,
          "refusal preserves descriptor");
  };
  s.intent.kind = Kind::unknown; check(s,"MGA.COW.INVALID_KIND",101000);
  s = Valid(); s.intent.transaction.local_id.value = 0; check(s,"MGA.COW.INVALID_TRANSACTION_IDENTITY",101001);
  s = Valid(); s.intent.row.row_uuid.value.bytes.fill(0); check(s,"MGA.COW.INVALID_ROW_IDENTITY",101002);
  s = Valid(); s.intent.kind = Kind::insert; check(s,"MGA.COW.INSERT_HAS_BASE",101003);
  s = Valid(); s.intent.has_base_version = false; check(s,"MGA.COW.BASE_REQUIRED",101004);
  s = Valid(); s.intent.base_version_sequence = 0; check(s,"MGA.COW.INVALID_BASE_SEQUENCE",101005);
  s = Valid(); s.intent.new_version_sequence = 0; check(s,"MGA.COW.INVALID_NEW_SEQUENCE",101006);
  s = Valid(); s.intent.new_version_sequence = 1; check(s,"MGA.COW.NONINCREASING_SEQUENCE",101007);
  s = Valid(); s.phase = Phase::unknown; check(s,"MGA.COW.INVALID_PHASE",101008);
  s = Valid(); s.resulting_row_state = RowState::unknown; check(s,"MGA.COW.INVALID_ROW_STATE",101009);
  s = Valid(); s.phase = Phase::published; check(s,"MGA.COW.EVIDENCE_REQUIRED",101010);
  Failure(mga::AdvanceCopyOnWriteMutationPhase(Valid(),Phase::published),"MGA.COW.ILLEGAL_TRANSITION",101011);

  // Precedence: invalid source before edge, edge before candidate validation.
  s = Valid(); s.phase = Phase::unknown; s.resulting_row_state = RowState::unknown;
  Failure(mga::AdvanceCopyOnWriteMutationPhase(s,Phase::published),"MGA.COW.INVALID_PHASE",101008);
  s = Valid(); s.phase = Phase::publish_pending_transaction;
  Failure(mga::AdvanceCopyOnWriteMutationPhase(s,Phase::published),"MGA.COW.EVIDENCE_REQUIRED",101010);
  s.evidence_record_written = true;
  Check(mga::AdvanceCopyOnWriteMutationPhase(s,Phase::published).ok(), "evidence positive control");

  mga::TransactionInventoryEntry entry;
  entry.identity = Valid().intent.transaction;
  const auto plan = [&] { return mga::PlanLocalCopyOnWriteMutationForTransaction(entry,Valid().intent.row,Kind::update,1,2); };
  entry.state = TxState::prepared; Failure(plan(),"MGA.COW.TRANSACTION_NOT_WRITABLE",101012);
  entry.state = TxState::read_only_active; Failure(plan(),"MGA.COW.READ_ONLY_TRANSACTION",101014);
  entry.state = TxState::active; Check(plan().ok(), "active inventory planning positive");
  entry.rollback_only = true; Failure(plan(),"MGA.COW.TRANSACTION_NOT_WRITABLE",101012);
  entry.rollback_only = false;
  entry.identity.local_id.value = 0; Failure(plan(),"MGA.COW.INVALID_TRANSACTION_IDENTITY",101001);
  entry.state = TxState::prepared; Failure(plan(),"MGA.COW.TRANSACTION_NOT_WRITABLE",101012);
  entry.identity = Valid().intent.transaction;

  mga::RowVersionMetadata metadata;
  metadata.identity.row = Valid().intent.row;
  metadata.identity.creator_transaction = Valid().intent.transaction;
  metadata.identity.version_sequence = 2;
  metadata.state = RowState::committed;
  metadata.creator_transaction_state = TxState::committed;
  metadata.payload_present = true;
  const auto assessed = mga::EvaluateCleanupEligibility(metadata,{});
  Assessment(assessed,"MGA.COW.CLEANUP_AUTHORITY_REQUIRED");
  Check(assessed.decision == mga::CleanupEligibilityDecision::eligible_requires_authority,
        "empty horizon list never claims authority");
  auto bad_metadata = metadata; bad_metadata.identity.version_sequence = 0;
  const auto invalid = mga::EvaluateCleanupEligibility(bad_metadata,{});
  Failure(invalid.status,invalid.diagnostic,"MGA.COW.INVALID_ROW_METADATA",101013);
  Check(invalid.decision == mga::CleanupEligibilityDecision::unknown, "invalid metadata has no decision");
  for (const auto state : {RowState::limbo,RowState::recovery_required}) {
    auto held = metadata; held.state = state;
    Assessment(mga::EvaluateCleanupEligibility(held,{}),"MGA.COW.CLEANUP_HELD");
  }
  for (unsigned hold = 0; hold <= static_cast<unsigned>(mga::CleanupHoldKind::unknown); ++hold)
    for (bool authoritative : {false,true}) for (unsigned horizon : {0u,7u,8u}) {
      mga::CleanupHorizonVector horizons;
      horizons.horizons.push_back({static_cast<mga::CleanupHoldKind>(hold),{horizon},authoritative,"PRIVATE-HORIZON-LABEL"});
      const auto r = mga::EvaluateCleanupEligibility(metadata,horizons);
      Assessment(r,authoritative && horizon == 8 && hold < static_cast<unsigned>(mga::CleanupHoldKind::unknown)
          ? "MGA.COW.CLEANUP_AUTHORITY_REQUIRED" : "MGA.COW.CLEANUP_HELD");
    }
  std::size_t enum_cases = 0;
  for (std::uint32_t value = 0; value != 65536; ++value) {
    s = Valid(); s.intent.kind = static_cast<Kind>(value);
    if (value == 0) { s.intent.has_base_version = false; s.intent.base_version_sequence = 0; }
    const auto kind = mga::ValidateCopyOnWriteMutationState(s);
    Check(kind.ok() == (value < 4), "complete uint16 mutation-kind domain"); ++enum_cases;
    s = Valid(); s.phase = static_cast<Phase>(value); s.evidence_record_written = true;
    Check(mga::ValidateCopyOnWriteMutationState(s).ok() == (value < 9), "complete uint16 phase domain"); ++enum_cases;
    s = Valid(); s.resulting_row_state = static_cast<RowState>(value);
    Check(mga::ValidateCopyOnWriteMutationState(s).ok() == (value >= 1 && value <= 7), "complete uint16 row-state domain"); ++enum_cases;
    entry.state = static_cast<TxState>(value);
    const auto r = plan();
    Check(r.ok() == (entry.state == TxState::active), "complete uint16 inventory state domain"); ++enum_cases;
  }
  Check(enum_cases == 262144, "all expected enum cases executed");
  for (unsigned value : {0u,8u,14u,65535u}) {
    auto malformed = metadata; malformed.state = static_cast<RowState>(value);
    const auto r = mga::EvaluateCleanupEligibility(malformed,{});
    Failure(r.status,r.diagnostic,"MGA.COW.INVALID_ROW_METADATA",101013);
  }
  for (unsigned value : {0u,14u,65535u}) {
    auto malformed = metadata; malformed.creator_transaction_state = static_cast<TxState>(value);
    const auto r = mga::EvaluateCleanupEligibility(malformed,{});
    Failure(r.status,r.diagnostic,"MGA.COW.INVALID_ROW_METADATA",101013);
  }
  for (unsigned scope : {0u,1u,2u,65535u}) {
    s = Valid(); s.intent.transaction.scope = static_cast<mga::TransactionScope>(scope);
    Check(mga::ValidateCopyOnWriteMutationState(s).ok() == (scope < 2), "exact transaction scope domain");
  }
  for (unsigned version = 0; version < 16; ++version) for (unsigned variant = 0; variant < 4; ++variant) {
    s = Valid(); auto& bytes = s.intent.transaction.transaction_uuid.value.bytes;
    bytes[6] = static_cast<unsigned char>(version << 4); bytes[8] = static_cast<unsigned char>(variant << 6);
    Check(mga::ValidateCopyOnWriteMutationState(s).ok() == (version == 7 && variant == 2), "system UUID version and variant policy");
  }
  std::cout << (failures ? "FAIL" : "PASS") << " enum_cases=" << enum_cases
            << " checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
