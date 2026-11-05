// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_state_restart.hpp"

#include <algorithm>

namespace scratchbird::storage::page {
namespace {
using E = TipPageError;

bool MarkDead(TipStateCode code) {
  switch (code) {
    case TipStateCode::unused:
    case TipStateCode::starting:
    case TipStateCode::active:
    case TipStateCode::read_only_active:
    case TipStateCode::rolling_back: return true;
    default: return false;
  }
}
bool Unresolved(TipStateCode code) { return code == TipStateCode::committing || code == TipStateCode::recovering; }
}  // namespace

TipRestartPlan PlanTransactionStateRestart(std::span<const TipChainPage> chain) {
  TipRestartPlan plan;
  if (chain.empty()) return plan;
  // Validate the whole chain before changing anything.
  std::vector<TipPageView> views;
  views.reserve(chain.size());
  for (std::size_t i = 0; i < chain.size(); ++i) {
    const auto d = DecodeTransactionStatePage(chain[i].image);
    if (!d.ok()) {
      plan.error = d.error;
      return plan;
    }
    if (d.view->tip_sequence != i || d.view->header.page_number != chain[i].page_number) {
      plan.error = E::invalid_link;
      return plan;
    }
    const u64 expected_link = i + 1 < chain.size() ? chain[i + 1].page_number : 0;
    // The last page may link one formatted spare that is not passed in.
    if (i + 1 < chain.size() && d.view->next_tip_page != expected_link) {
      plan.error = E::invalid_link;
      return plan;
    }
    views.push_back(*d.view);
  }
  const TipCounters counters = *views.front().counters;
  for (std::size_t i = 1; i < views.size(); ++i) {
    const auto below = CheckTransactionStateSlotsBelowNext(views[i], counters.next_transaction);
    if (!below.ok()) {
      plan.error = below.error;
      return plan;
    }
  }
  const u64 next = counters.next_transaction;
  const u64 from = std::max<u64>(1, counters.horizons.oldest_active);
  plan.next_transaction = next;
  plan.scanned_from = from;
  // Pages must cover every number below Next.
  const u64 covered = views.back().first_number + views.back().slots;
  if (covered < next) {
    plan.error = E::invalid_counters;
    return plan;
  }
  // Work on copies; the caller's images change only after the whole pass succeeds.
  std::vector<std::vector<byte>> work(chain.size());
  for (std::size_t i = 0; i < chain.size(); ++i) {
    const auto& v = views[i];
    if (v.first_number + v.slots <= from || v.first_number >= next) continue;
    const std::span<const byte> before = chain[i].image;
    auto& image = work[i];
    image.assign(before.begin(), before.end());
    const u32 lo = static_cast<u32>(from > v.first_number ? from - v.first_number : 0);
    const u32 hi = static_cast<u32>(std::min<u64>(v.slots, next - v.first_number));
    for (u32 slot = lo; slot < hi; ++slot) {
      const auto word = TipWordAt(v, slot);
      if (!word) {  // decoded above, so unreachable; refuse rather than guess
        plan.error = E::invalid_word;
        return plan;
      }
      if (Unresolved(word->state)) plan.unresolved.push_back(v.first_number + slot);
      if (!MarkDead(word->state)) continue;
      const auto edit = SetTransactionStateCode(image, slot, TipStateCode::dead);
      if (!edit.ok()) {
        plan.error = edit.error;
        return plan;
      }
      ++plan.marked_dead;
    }
    // One write per changed unit, with the unit as it was before this pass.
    for (std::size_t at = kTransactionStateWordsOffset; at < before.size(); at += kTransactionStateWriteUnitBytes) {
      if (std::equal(before.begin() + at, before.begin() + at + kTransactionStateWriteUnitBytes,
                     image.begin() + at))
        continue;
      TipUnitWrite unit;
      unit.page_number = chain[i].page_number;
      unit.unit_offset = static_cast<u32>(at);
      std::copy_n(before.begin() + at, kTransactionStateWriteUnitBytes, unit.preimage.begin());
      plan.units.push_back(unit);
    }
  }
  for (std::size_t i = 0; i < chain.size(); ++i)
    if (!work[i].empty()) std::copy(work[i].begin(), work[i].end(), chain[i].image.begin());
  plan.error = E::none;
  return plan;
}

}  // namespace scratchbird::storage::page
