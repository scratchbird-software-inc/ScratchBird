// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "session_ledger_chain.hpp"

#include "careful_chain_support.hpp"

#include <limits>
#include <new>

namespace scratchbird::storage::page {
namespace {
namespace disk = scratchbird::storage::disk;
using E = TransactionWriterError;
using disk::CarefulWriteError;
using Index = std::map<Uuid, SessionLedgerEntry>;

// The reference rules of one record against an index, changing nothing.
// `bound` is the index's record bound.
E CheckRules(const Index& index, const SessionLedgerRecord& record, std::size_t bound) {
  const auto existing = index.find(record.record_uuid);
  const auto find = [&](const Uuid& id) -> const SessionLedgerEntry* {
    const auto at = index.find(id);
    return at == index.end() ? nullptr : &at->second;
  };
  // A session reference: complete, a session, not ended.
  const auto session_ref = [&](const Uuid& id) -> E {
    const auto* s = find(id);
    if (!s) return E::ledger_unknown_session;
    if (s->kind != SessionLedgerKind::session) return E::ledger_wrong_kind;
    if (s->ended) return E::ledger_session_ended;
    return E::none;
  };
  if (const auto* s = std::get_if<LedgerSessionRecord>(&record.body)) {
    if (existing != index.end()) {
      // The one repeat: this session's end record.
      const auto& old = existing->second;
      if (!(old.kind == SessionLedgerKind::session && !old.ended && s->ended() && old.ref == s->principal_uuid))
        return E::ledger_duplicate_record;
      return E::none;
    }
    if (s->ended()) return E::ledger_unknown_session;  // an end record needs its open record
  } else if (existing != index.end()) {
    return E::ledger_duplicate_record;
  }
  if (index.size() >= bound && existing == index.end()) return E::ledger_full;
  if (const auto* a = std::get_if<LedgerAttachmentRecord>(&record.body)) {
    if (const auto e = session_ref(a->session_uuid); e != E::none) return e;
    const auto* c = find(a->connection_uuid);
    if (!c) return E::ledger_unknown_connection;
    if (c->kind != SessionLedgerKind::connection) return E::ledger_wrong_kind;
    if (!a->predecessor_uuid.is_nil()) {
      const auto* p = find(a->predecessor_uuid);
      if (!p || p->kind != SessionLedgerKind::attachment) return E::ledger_unknown_predecessor;
      if (!(p->ref == a->session_uuid)) return E::ledger_predecessor_mismatch;
    }
  } else if (const auto* c = std::get_if<LedgerSettingsChangeRecord>(&record.body)) {
    if (const auto e = session_ref(c->session_uuid); e != E::none) return e;
    if (c->generation != find(c->session_uuid)->settings_generation + 1) return E::ledger_generation_order;
  }
  return E::none;
}

// Applies a record the rules above admitted.
void ApplyRecord(Index& index, const SessionLedgerRecord& record) {
  SessionLedgerEntry entry;
  entry.ordinal = record.ordinal;
  entry.kind = record.kind();
  if (const auto* s = std::get_if<LedgerSessionRecord>(&record.body)) {
    entry.ref = s->principal_uuid;
    entry.ended = s->ended();
    const auto at = index.find(record.record_uuid);
    if (at != index.end()) entry.settings_generation = at->second.settings_generation;  // the end record keeps it
  } else if (const auto* a = std::get_if<LedgerAttachmentRecord>(&record.body)) {
    entry.ref = a->session_uuid;
  } else if (const auto* c = std::get_if<LedgerSettingsChangeRecord>(&record.body)) {
    entry.ref = c->session_uuid;
    index.find(c->session_uuid)->second.settings_generation = c->generation;
  }
  index[record.record_uuid] = entry;
}
}  // namespace

SessionLedger::SessionLedger(const SessionLedgerBinding& binding) noexcept : binding_(binding) {
  const auto* profile = disk::FindCanonicalFilespacePageProfile(binding.root.page_size_profile_uuid);
  if (profile && binding.root.kind == 25 && binding.root.page_type == kTransactionStatePageType &&
      binding.root.page_number != 0 && binding.root.page_generation != 0 && !binding.database_uuid.is_nil() &&
      binding.maximum_chain_pages != 0 && binding.maximum_records != 0 &&
      SessionLedgerSlotsPerPage(profile->page_size_bytes) != 0)
    page_size_ = profile->page_size_bytes;
}

const SessionLedgerEntry* SessionLedger::Find(const Uuid& record_uuid) const noexcept {
  const auto at = index_.find(record_uuid);
  return at == index_.end() ? nullptr : &at->second;
}

TransactionWriterError SessionLedger::CheckHeaderCitation(const Uuid& attachment, const Uuid& session,
                                                          const Uuid& principal, bool allow_ended_session) const noexcept {
  if (attachment.is_nil() && session.is_nil()) return E::none;
  if (!loaded_) return E::ledger_not_loaded;
  const SessionLedgerEntry* a = nullptr;
  if (!attachment.is_nil()) {
    a = Find(attachment);
    if (!a) return E::ledger_unknown_attachment;
    if (a->kind != SessionLedgerKind::attachment) return E::ledger_wrong_kind;
  }
  if (!session.is_nil()) {
    const auto* s = Find(session);
    if (!s) return E::ledger_unknown_session;
    if (s->kind != SessionLedgerKind::session) return E::ledger_wrong_kind;
    if (s->ended && !allow_ended_session) return E::ledger_session_ended;
    if (!(s->ref == principal)) return E::ledger_principal_mismatch;
    if (a && !(a->ref == session)) return E::ledger_attachment_session_mismatch;
  }
  return E::none;
}

SessionLedgerLoadResult SessionLedger::Load(disk::CarefulWriteSink& sink) noexcept {
  SessionLedgerLoadResult result;
  loaded_ = false;
  if (!page_size_) {
    result.error = E::invalid_request;
    return result;
  }
  if (failed_) {
    result.error = E::writer_failed;
    return result;
  }
  try {
    Index index;
    std::vector<u64> pages;
    std::vector<byte> buffer;
    u64 records = 0;
    disk::NativeCommonPageHeaderBinding first;
    first.filespace = {binding_.database_uuid, binding_.root.filespace_uuid, binding_.root.page_size_profile_uuid};
    first.page_number = binding_.root.page_number;
    first.page_generation = binding_.root.page_generation;
    first.page_type = kTransactionStatePageType;
    u64 page = binding_.root.page_number;
    const auto invalid = [&](u64 at_page, TipPageError page_error, std::optional<u32> slot) {
      result.error = E::ledger_invalid;
      result.failing_page = at_page;
      result.page_error = page_error;
      if (slot) result.failing_ordinal = records + *slot;
      return result;
    };
    for (std::size_t i = 0; page != 0; ++i) {
      if (i >= binding_.maximum_chain_pages) return invalid(page, TipPageError::invalid_link, std::nullopt);
      buffer.clear();
      if (!detail::ReadChainPage(sink, page_size_, page, buffer, result)) return result;
      const auto d = DecodeSessionLedgerPage(buffer, i == 0 ? &first : nullptr);
      if (!d.ok()) {
        result.record_error = d.record_error;
        return invalid(page, d.error, d.slot);
      }
      const auto& v = *d.view;
      if (v.sequence != i || v.header.page_number != page || v.first_ordinal != records ||
          v.header.database_uuid != binding_.database_uuid || v.header.filespace_uuid != binding_.root.filespace_uuid ||
          v.header.page_size_profile_uuid != binding_.root.page_size_profile_uuid)
        return invalid(page, TipPageError::invalid_link, std::nullopt);
      for (u32 slot = 0; slot < v.used; ++slot) {
        const auto record = SessionLedgerRecordAt(v, slot);
        if (!record) return invalid(page, TipPageError::invalid_word, slot);
        if (const auto rule = CheckRules(index, *record, binding_.maximum_records); rule != E::none) {
          result.rule = rule;
          return invalid(page, TipPageError::invalid_slot, slot);
        }
        ApplyRecord(index, *record);
      }
      records += v.used;
      result.torn_tail = v.torn_slot.has_value();
      pages.push_back(page);
      page = v.next_page;
    }
    index_ = std::move(index);
    pages_ = std::move(pages);
    records_ = records;
    loaded_ = true;
    result.records = records;
    result.pages = pages_.size();
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) {
    result.error = E::resource_exhausted;
    return result;
  } catch (const std::length_error&) {
    result.error = E::resource_exhausted;
    return result;
  }
}

SessionLedgerAppendResult SessionLedger::Append(disk::CarefulWriteSink& sink, const SessionLedgerRecord& given) noexcept {
  using R = SessionLedgerAppendResult;
  const auto refuse = [](E error) {
    R r;
    r.error = error;
    return r;
  };
  if (!page_size_) return refuse(E::invalid_request);
  if (failed_) return refuse(E::writer_failed);
  if (binding_.read_only_database) return refuse(E::read_only_database);
  if (!loaded_) return refuse(E::ledger_not_loaded);
  try {
    SessionLedgerRecord record = given;
    record.database_uuid = binding_.database_uuid;
    record.ordinal = records_;
    if (const auto valid = ValidateSessionLedgerRecord(record); valid != SessionLedgerRecordError::none) {
      auto r = refuse(E::ledger_invalid_record);
      r.record_error = valid;
      r.ordinal = records_;
      return r;
    }
    if (const auto rule = CheckRules(index_, record, binding_.maximum_records); rule != E::none) {
      auto r = refuse(rule);
      r.ordinal = records_;
      return r;
    }
    R result;
    result.ordinal = records_;
    // The last page, as the file has it now.
    std::vector<byte> image;
    if (!detail::ReadChainPage(sink, page_size_, pages_.back(), image, result)) return result;
    auto decoded = DecodeSessionLedgerPage(image);
    const u64 slots = SessionLedgerSlotsPerPage(page_size_);
    const u64 expected_used = records_ - (pages_.size() - 1) * slots;
    if (!decoded.ok() || decoded.view->used != expected_used || decoded.view->next_page != 0) {
      auto r = refuse(E::ledger_invalid);  // changed underneath, or unreadable
      r.ordinal = records_;
      r.page_error = decoded.error;
      return r;
    }
    u64 target_page = pages_.back();
    if (decoded.view->used == decoded.view->slots) {
      // Full: grow by one page in its own plan, then write into the new page.
      if (pages_.size() >= binding_.maximum_chain_pages) return refuse(E::ledger_full);
      if (!binding_.allocation_root || !binding_.issuer) {
        auto r = refuse(E::chain_full);
        r.page_error = TipPageError::invalid_slot;
        r.ordinal = records_;
        return r;
      }
      detail::ChainGrowthSpec spec;
      spec.database_uuid = binding_.database_uuid;
      spec.allocation_root = *binding_.allocation_root;
      spec.issuer = binding_.issuer;
      spec.barrier_policy = binding_.barrier_policy;
      spec.page_size = page_size_;
      spec.page_type = kTransactionStatePageType;
      spec.owner_object = binding_.root.object_uuid;
      spec.filespace_uuid = binding_.root.filespace_uuid;
      spec.page_size_profile_uuid = binding_.root.page_size_profile_uuid;
      spec.last_image = image;
      spec.last_page_number = pages_.back();
      const u64 sequence = pages_.size();
      spec.format = [sequence](const disk::NativeCommonPageHeader& header, std::span<byte> page) {
        SessionLedgerPageFormat f;
        f.header = header;
        f.sequence = sequence;
        return FormatSessionLedgerPage(f, page);
      };
      spec.link = [](std::span<byte> last, u64 next_page) { return SetSessionLedgerNextPage(last, next_page); };
      detail::ChainGrowthOutcome grown;
      const bool ok = detail::GrowChainByOnePage(sink, spec, grown, failed_);
      result.growth_writes = grown.growth_writes;
      result.growth_syncs = grown.growth_syncs;
      if (!ok) {
        result.error = grown.error;
        result.page_error = grown.page_error;
        result.allocation_error = grown.allocation_error;
        result.write_error = grown.write_error;
        result.io = grown.io;
        return result;
      }
      const auto linked = DecodeSessionLedgerPage(image);  // the link edit is in this copy
      if (!linked.ok() || linked.view->next_page == 0) {
        auto r = refuse(E::ledger_invalid);
        r.page_error = linked.error;
        return r;
      }
      target_page = linked.view->next_page;
      pages_.push_back(target_page);  // the chain on disk is now one page longer, whatever follows
      image.clear();
      if (!detail::ReadChainPage(sink, page_size_, target_page, image, result)) return result;
      decoded = DecodeSessionLedgerPage(image);
      if (!decoded.ok() || decoded.view->used != 0 || decoded.view->sequence != pages_.size() - 1) {
        auto r = refuse(E::ledger_invalid);
        r.page_error = decoded.error;
        return r;
      }
    }
    const auto edit = AppendSessionLedgerRecord(image, record);
    if (!edit.ok() || !edit.changed) {
      auto r = refuse(E::ledger_invalid);
      r.page_error = edit.error;
      return r;
    }
    disk::CarefulWritePlan plan;
    const auto node = detail::AddChainUnit(plan, target_page, page_size_, image, edit);
    if (!node.ok()) return refuse(E::resource_exhausted);
    if (!detail::RunChainPlan(plan, sink, binding_.barrier_policy, result, failed_)) return result;
    // Durable (written and synced): only now is the record indexed and citable.
    ApplyRecord(index_, record);
    ++records_;
    result.error = E::none;
    return result;
  } catch (const std::bad_alloc&) {
    return refuse(E::resource_exhausted);
  } catch (const std::length_error&) {
    return refuse(E::resource_exhausted);
  }
}

}  // namespace scratchbird::storage::page
