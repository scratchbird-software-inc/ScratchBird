// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_blob.hpp"
#include <array>
#include <cstdint>
#include <iostream>
namespace scratchbird::core::datatypes {
class BlobLifetimeRuntimeConformanceAccessV3 final {
 public:
  static bool Configure(BlobLifetimeBudgetLedgerV3Generation1& l,
      const BlobLifetimeBudgetConfigurationV3Generation1& c) noexcept {
    return l.Configure(c);
  }
  static bool Freeze(BlobLifetimeBudgetLedgerV3Generation1& l) noexcept {
    return l.Freeze();
  }
  static auto Snapshot(BlobLifetimeBudgetLedgerV3Generation1& l) noexcept {
    return l.SnapshotForConformance();
  }
  static bool Reserve(BlobLifetimeBudgetLedgerV3Generation1& l,
      std::uint64_t ordinary, std::uint64_t cleanup,
      std::uint64_t* available) noexcept {
    return l.ReserveOrdinaryAndCleanup(ordinary, cleanup, available);
  }
  static auto Consume(BlobLifetimeBudgetLedgerV3Generation1& l,
      std::uint64_t amount) noexcept { return l.ConsumeCleanup(amount); }
  static bool Cancel(BlobLifetimeBudgetLedgerV3Generation1& l,
      std::uint64_t amount) noexcept { return l.CancelCleanup(amount); }
};
}
using namespace scratchbird::core::datatypes;
static bool same(const BlobLifetimeBudgetSnapshotV3Generation1& a,
                 const BlobLifetimeBudgetSnapshotV3Generation1& b) {
  if (a.configured != b.configured || a.frozen != b.frozen ||
      a.terminal_quarantined != b.terminal_quarantined ||
      a.scratch_data != b.scratch_data || a.scratch_size != b.scratch_size ||
      a.admitted_window_octets != b.admitted_window_octets ||
      a.admitted_relative_timeout_ns != b.admitted_relative_timeout_ns)
    return false;
  for (std::size_t i=0;i<a.counters.size();++i)
    if (a.counters[i].limit != b.counters[i].limit ||
        a.counters[i].invoked != b.counters[i].invoked ||
        a.counters[i].reserved_cleanup != b.counters[i].reserved_cleanup)
      return false;
  return true;
}
int main() {
  std::array<byte,37> scratch{};
  BlobLifetimeBudgetConfigurationV3Generation1 cfg{};
  for (std::size_t i=0;i<cfg.limits.size();++i) cfg.limits[i]=100+i;
  cfg.scratch=scratch; cfg.admitted_window_octets=4096;
  cfg.admitted_relative_timeout_ns=987654321;
  BlobLifetimeBudgetLedgerV3Generation1 ledger;
  auto pristine=BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  if (pristine.configured || pristine.frozen || pristine.terminal_quarantined)
    return 1;
  std::uint64_t available=999;
  if (BlobLifetimeRuntimeConformanceAccessV3::Reserve(ledger,1,1,&available))
    return 2;
  if (!same(pristine,BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)))
    return 3;
  if (!BlobLifetimeRuntimeConformanceAccessV3::Configure(ledger,cfg)) return 4;
  auto configured=BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  if (!configured.configured || configured.frozen ||
      configured.scratch_data!=scratch.data() ||
      configured.scratch_size!=scratch.size() ||
      configured.admitted_window_octets!=4096 ||
      configured.admitted_relative_timeout_ns!=987654321) return 5;
  for (std::size_t i=1;i<configured.counters.size();++i)
    if (configured.counters[i].limit!=99+i) return 6;
  auto changed=cfg; changed.limits[0]++;
  if (BlobLifetimeRuntimeConformanceAccessV3::Configure(ledger,changed)) return 7;
  if (!same(configured,BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)))
    return 8;
  if (!BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger)) return 9;
  auto frozen=BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  if (!frozen.frozen) return 10;
  if (BlobLifetimeRuntimeConformanceAccessV3::Freeze(ledger)) return 11;
  if (!same(frozen,BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)))
    return 12;
  if (BlobLifetimeRuntimeConformanceAccessV3::Configure(ledger,cfg)) return 13;
  if (!same(frozen,BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger)))
    return 14;
  if (!BlobLifetimeRuntimeConformanceAccessV3::Reserve(ledger,1,1,&available))
    return 15;
  auto used=BlobLifetimeRuntimeConformanceAccessV3::Snapshot(ledger);
  if (used.counters[1].invoked!=1 || used.counters[1].reserved_cleanup!=1)
    return 16;

  BlobLifetimeBudgetLedgerV3Generation1 corrupt;
  if (!BlobLifetimeRuntimeConformanceAccessV3::Configure(corrupt,cfg) ||
      !BlobLifetimeRuntimeConformanceAccessV3::Freeze(corrupt)) return 17;
  const auto missing=BlobLifetimeRuntimeConformanceAccessV3::Consume(corrupt,1);
  const auto after=BlobLifetimeRuntimeConformanceAccessV3::Snapshot(corrupt);
  if (missing.obligation_consumed || missing.cleanup_callback_charge_commit ||
      !missing.ledger_terminal_quarantined || !after.terminal_quarantined)
    return 18;
  BlobLifetimeBudgetLedgerV3Generation1 cancel_corrupt;
  if (!BlobLifetimeRuntimeConformanceAccessV3::Configure(cancel_corrupt,cfg) ||
      !BlobLifetimeRuntimeConformanceAccessV3::Freeze(cancel_corrupt))
    return 19;
  if (BlobLifetimeRuntimeConformanceAccessV3::Cancel(cancel_corrupt,1) ||
      !BlobLifetimeRuntimeConformanceAccessV3::Snapshot(cancel_corrupt)
           .terminal_quarantined)
    return 20;
  std::cout << "configure_freeze=PASS all_11=PASS immutable_refusal=PASS "
               "post_freeze_use=PASS missing_reservation_terminal=PASS "
               "cancel_missing_terminal=PASS\n";
}
