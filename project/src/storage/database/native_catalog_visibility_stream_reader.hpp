// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// SEARCH_KEY: SB_NATIVE_CATALOG_STREAM (owner 2026-11-25: "a streaming reader/writer is a high priority").
//
// A visitor over the visible rows of one catalog relation, read one page at a time. The collect-everything readers
// (NativeCatalogRelationLeaseReader / NativeCatalogVisibilityLeaseReader) hold every base leaf of the relation, every
// index page and six arrays of maximum_rows entries at once, and refuse (resource_exhausted) past the scan tier's
// 4096 rows / 4 MiB. This reader has NO row or byte cap of its own:
//   - the relation's B-tree is walked by a cursor that holds at most kNativeCatalogStreamTreeDepth levels, two pages
//     each (the page just read at a level and the one before it, which the sibling and fence checks compare with);
//   - base leaves come through a window of `window_pages` decoded leaves (least recently used, pinned while a version
//     is being judged);
//   - a row with ONE version (the overwhelming majority: a first version with no forward link) is judged at once from
//     its own page and handed to the visitor;
//   - a row with more than one version is the one place a row's versions are on different pages (a successor is
//     placed on any leaf the head reaches, with a forward link on its predecessor), so the versions of such rows are
//     registered as pages pass (version UUID, row UUID, row version, page reference: about 100 bytes each) and the
//     rows are judged after the last page. That registry is O(versions of rows that have history), the MGA history a
//     sweep removes, not O(rows); `history_versions` bounds it and a pass that fills it ends
//     NativeCatalogStreamError::history_exhausted (a memory-budget refusal with its own code).
// Visibility is decided by the one decision function (mga::ObserveVersionEffectVisibility) with the inputs the
// collect-everything reader builds (NativeCatalogVisibilityLeaseReader::VersionMetadataOf / CommittedSnapshotOf /
// NameOf, shared, not copied); the snapshot-point view entries judge by a snapshot VALUE exactly as ReadCommittedAt does.
// Memory: everything the pass holds is prepared up front from the limits and charged to the governed storage memory
// (PrepareNativeCatalogStreamRead); a visitor that retains rows spends its own memory, which is the caller's to bound.
//
// Checks that stay in a pass (they are per page, per cell or per row): page seals, tree topology, fences and sibling
// links, split lag, cell <-> row binding (row UUID, creator), creator resolution and local scope, cluster authority
// refusal, the metadata and name-envelope decodes, the per-version metadata observation, navigation-page creators
// committed, and, among the versions of one multi-version row, the chain and metadata-succession rules.
// Checks that are relation-wide and therefore NOT in a pass (they need every version at once): a version UUID or an
// object UUID repeated across rows, a previous-version link naming another row's version, an index page seen twice.
// They stay with the collect-everything reader (open verification), see the report.
// Order: rows with one version in index order, then rows with history in row-UUID order.
#include "native_catalog_visibility_lease_reader.hpp"
#include <memory>

namespace scratchbird::storage::database {
inline constexpr usize kNativeCatalogStreamTreeDepth = 6;

struct NativeCatalogStreamLimits {
  Uuid maximum_profile;
  usize maximum_devices = 1;
  usize window_pages = 4;        // decoded base leaves resident at once (at least 3)
  usize history_versions = 1024;  // versions of rows with history held until the pass ends
};
enum class NativeCatalogStreamError {
  none, invalid_request, memory_failure, root_failure, missing_relation, binding_mismatch, tree_failure, leaf_failure,
  invalid_locator, creator_mismatch, cluster_requires_authority, navigation_not_committed, invalid_chain, missing_version,
  visibility_failure, requires_recovery, name_payload_invalid, io_failure, resource_exhausted,
  history_exhausted,  // the governed history registry filled: rows with more than one version exceed history_versions
  snapshot_failure,
  hook_refused  // SB_OPEN_COHORT_STREAM: a hook (the open's cohort verifier) refused; the hook holds its own detail
};
inline const char* NativeCatalogStreamErrorName(NativeCatalogStreamError e) noexcept {
  using E=NativeCatalogStreamError;
  switch(e){
    case E::none:return "none";case E::invalid_request:return "invalid_request";case E::memory_failure:return "memory_failure";
    case E::root_failure:return "root_failure";case E::missing_relation:return "missing_relation";
    case E::binding_mismatch:return "binding_mismatch";case E::tree_failure:return "tree_failure";
    case E::leaf_failure:return "leaf_failure";case E::invalid_locator:return "invalid_locator";
    case E::creator_mismatch:return "creator_mismatch";case E::cluster_requires_authority:return "cluster_requires_authority";
    case E::navigation_not_committed:return "navigation_not_committed";case E::invalid_chain:return "invalid_chain";
    case E::missing_version:return "missing_version";case E::visibility_failure:return "visibility_failure";
    case E::requires_recovery:return "requires_recovery";case E::name_payload_invalid:return "name_payload_invalid";
    case E::io_failure:return "io_failure";case E::resource_exhausted:return "resource_exhausted";
    case E::history_exhausted:return "history_exhausted";case E::snapshot_failure:return "snapshot_failure";
    case E::hook_refused:return "hook_refused";
  }
  return "unknown";
}
namespace stream_detail {
struct StreamFile {
  disk::NativeFilespaceDevice source;
  u64 total_pages = 0;
  u32 size = 0, flags = 0, role = 0;
};
struct StreamPending {
  disk::NativePageReference ref;
  bool root = false;
  u16 level = 0;
  std::optional<page::NativeBtreeKeyView> low, high;
  bool unlisted = false;
};
struct StreamTreeSlot {
  page::NativeBtreePageViewResult page;
  std::span<byte> image;
  std::span<page::NativeBtreeCellView> cells;
  std::span<byte> keys;
};
// One version of a row that has more than one: where it is, and whether the index lists it.
struct HistoryVersion {
  Uuid row_uuid, version_uuid;
  u64 row_version = 0;
  disk::NativePageReference page;
  bool candidate = false;
};
struct WindowSlot {
  disk::NativePageReference ref;
  NativeCatalogLeafPageView page;
  u64 tick = 0;
  u32 pins = 0;
  bool valid = false, scanned = false;
};
}  // namespace stream_detail

inline usize NativeCatalogStreamArenaBytes(const NativeCatalogStreamLimits& l) noexcept {
  const auto* profile = disk::FindCanonicalFilespacePageProfile(l.maximum_profile);
  if (!profile || !l.maximum_devices || l.window_pages < 3 || !l.history_versions) return 0;
  const usize largest = profile->page_size_bytes;
  const auto req = page::NativeBtreePageViewBackingRequirements(largest);
  constexpr usize slots = 2 * kNativeCatalogStreamTreeDepth + 1;
  usize bytes = 0;
  const auto add = [&](usize n, usize width, usize pad) {
    constexpr auto max = std::numeric_limits<usize>::max();
    if (n && width > (max - pad) / n) return false;
    const auto amount = n * width + pad;
    if (amount > max - bytes) return false;
    bytes += amount;
    return true;
  };
  if (!add(l.maximum_devices, sizeof(disk::NativeFilespaceDevice), alignof(disk::NativeFilespaceDevice)) ||
      !add(l.maximum_devices, sizeof(stream_detail::StreamFile), alignof(stream_detail::StreamFile)) ||
      !add(slots, sizeof(stream_detail::StreamTreeSlot), alignof(stream_detail::StreamTreeSlot)) ||
      !add(slots * largest, 1, largest) || !add(slots * req.key_bytes, 1, 1) ||
      !add(slots * req.cells, sizeof(page::NativeBtreeCellView), alignof(page::NativeBtreeCellView)) ||
      !add(kNativeCatalogStreamTreeDepth * (req.cells + 2), sizeof(stream_detail::StreamPending), alignof(stream_detail::StreamPending)) ||
      !add(req.reference_slots, sizeof(page::NativeBtreeReferenceSlot), alignof(page::NativeBtreeReferenceSlot)) ||
      !add(32, sizeof(disk::FilespaceRootReference), alignof(disk::FilespaceRootReference)) || !add(l.maximum_devices * largest, 1, largest) ||
      !add(largest, 1, largest) ||
      !add(l.window_pages, sizeof(stream_detail::WindowSlot), alignof(stream_detail::WindowSlot)) ||
      !add(l.window_pages, sizeof(NativeCatalogLeafPreparedMemory), alignof(NativeCatalogLeafPreparedMemory)) ||
      !add(l.history_versions, sizeof(stream_detail::HistoryVersion), alignof(stream_detail::HistoryVersion)) ||
      !add(2 * l.history_versions + 1, sizeof(u32), alignof(u32)))
    return 0;
  return bytes;
}
// The governed total: this pass's arena, the roots reader's workspace and one decoded-leaf workspace per window slot.
inline usize NativeCatalogStreamWorkspaceBytes(const NativeCatalogStreamLimits& l) noexcept {
  const auto arena = NativeCatalogStreamArenaBytes(l);
  const auto roots = NativeCatalogRootsWorkspaceBytes(l.maximum_profile);
  const auto leaf = NativeCatalogLeafWorkspaceBytes(l.maximum_profile);
  if (!arena || !roots || !leaf) return 0;
  return arena + roots + l.window_pages * leaf;
}

class NativeCatalogVisibilityStreamReader;
struct NativeCatalogStreamPreparedResult;
class NativeCatalogStreamPreparedMemory {
 public:
  NativeCatalogStreamPreparedMemory() noexcept = default;
  ~NativeCatalogStreamPreparedMemory() { Clear(); }
  NativeCatalogStreamPreparedMemory(const NativeCatalogStreamPreparedMemory&) = delete;
  NativeCatalogStreamPreparedMemory& operator=(const NativeCatalogStreamPreparedMemory&) = delete;
  NativeCatalogStreamPreparedMemory(NativeCatalogStreamPreparedMemory&& o) noexcept { Swap(o); }
  NativeCatalogStreamPreparedMemory& operator=(NativeCatalogStreamPreparedMemory&& o) noexcept {
    if (this != &o) { Clear(); Swap(o); }
    return *this;
  }
  explicit operator bool() const noexcept { return bool(arena_) && bool(roots_) && !leaves_.empty(); }
  usize workspace_bytes() const noexcept { return workspace_bytes_; }
  const NativeCatalogStreamLimits& limits() const noexcept { return limits_; }

 private:
  void Clear() noexcept {
    for (auto& l : leaves_) std::destroy_at(&l);
    leaves_ = {};
    arena_ = {};
    roots_ = {};
  }
  void Swap(NativeCatalogStreamPreparedMemory& o) noexcept {
    using std::swap;
    swap(arena_, o.arena_); swap(roots_, o.roots_); swap(leaves_, o.leaves_); swap(window_, o.window_);
    swap(limits_, o.limits_); swap(workspace_bytes_, o.workspace_bytes_); swap(largest_, o.largest_);
    swap(devices_, o.devices_); swap(files_, o.files_); swap(slots_, o.slots_); swap(pending_, o.pending_);
    swap(references_, o.references_); swap(root_refs_, o.root_refs_); swap(zero_, o.zero_); swap(zero_check_, o.zero_check_);
    swap(history_, o.history_); swap(hash_, o.hash_);
  }
  NativeStorageArena arena_;
  NativeCatalogRootsPreparedMemory roots_;
  std::span<NativeCatalogLeafPreparedMemory> leaves_;
  std::span<stream_detail::WindowSlot> window_;
  NativeCatalogStreamLimits limits_;
  usize workspace_bytes_ = 0, largest_ = 0;
  std::span<disk::NativeFilespaceDevice> devices_;
  std::span<stream_detail::StreamFile> files_;
  std::span<stream_detail::StreamTreeSlot> slots_;
  std::span<stream_detail::StreamPending> pending_;
  std::span<page::NativeBtreeReferenceSlot> references_;
  std::span<disk::FilespaceRootReference> root_refs_;
  std::span<byte> zero_;        // one page-zero image per file, as read at the pass start (SB_STREAM_PASS_ANCHOR)
  std::span<byte> zero_check_;  // the end-of-pass re-read
  std::span<stream_detail::HistoryVersion> history_;
  std::span<u32> hash_;
  friend class NativeCatalogVisibilityStreamReader;
  friend NativeCatalogStreamPreparedResult PrepareNativeCatalogStreamRead(
      const NativeCatalogStreamLimits&, NativeStorageMemory&, const NativeStorageMemoryBinding&) noexcept;
};
struct NativeCatalogStreamPreparedResult {
  NativeCatalogStreamError error = NativeCatalogStreamError::invalid_request;
  NativeStorageMemoryError memory_error = NativeStorageMemoryError::none;
  NativeCatalogStreamPreparedMemory workspace;
  bool ok() const noexcept { return error == NativeCatalogStreamError::none && bool(workspace); }
};
inline NativeCatalogStreamPreparedResult PrepareNativeCatalogStreamRead(
    const NativeCatalogStreamLimits& l, NativeStorageMemory& memory, const NativeStorageMemoryBinding& binding) noexcept {
  using E = NativeCatalogStreamError;
  NativeCatalogStreamPreparedResult out;
  try {
    const auto total = NativeCatalogStreamWorkspaceBytes(l);
    if (!total) return out;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(l.maximum_profile);
    const usize largest = profile->page_size_bytes;
    const auto req = page::NativeBtreePageViewBackingRequirements(largest);
    NativeCatalogStreamPreparedMemory w;
    auto arena = memory.CreateArena(binding, NativeCatalogStreamArenaBytes(l), alignof(std::max_align_t));
    if (!arena.ok()) { out.error = E::memory_failure; out.memory_error = arena.error; return out; }
    w.arena_ = std::move(arena.arena);
    const auto array = [&]<class T>(usize n, usize alignment = alignof(T)) -> std::span<T> {
      static_assert(std::is_trivially_destructible_v<T>);
      if (!n) return {};
      auto block = w.arena_.Allocate(n * sizeof(T), alignment);
      if (!block.ok()) throw std::bad_alloc();
      auto* p = static_cast<T*>(block.pointer);
      std::uninitialized_value_construct_n(p, n);
      return {p, n};
    };
    constexpr usize slot_count = 2 * kNativeCatalogStreamTreeDepth + 1;
    w.devices_ = array.template operator()<disk::NativeFilespaceDevice>(l.maximum_devices);
    w.files_ = array.template operator()<stream_detail::StreamFile>(l.maximum_devices);
    w.slots_ = array.template operator()<stream_detail::StreamTreeSlot>(slot_count);
    const auto images = array.template operator()<byte>(slot_count * largest, largest);
    const auto keys = array.template operator()<byte>(slot_count * req.key_bytes, 1);
    const auto cells = array.template operator()<page::NativeBtreeCellView>(slot_count * req.cells);
    for (usize i = 0; i < slot_count; ++i) {
      w.slots_[i].image = images.subspan(i * largest, largest);
      w.slots_[i].keys = keys.subspan(i * req.key_bytes, req.key_bytes);
      w.slots_[i].cells = cells.subspan(i * req.cells, req.cells);
    }
    w.pending_ = array.template operator()<stream_detail::StreamPending>(kNativeCatalogStreamTreeDepth * (req.cells + 2));
    w.references_ = array.template operator()<page::NativeBtreeReferenceSlot>(req.reference_slots);
    w.root_refs_ = array.template operator()<disk::FilespaceRootReference>(32);
    w.zero_ = array.template operator()<byte>(l.maximum_devices * largest, largest);
    w.zero_check_ = array.template operator()<byte>(largest, largest);
    w.window_ = array.template operator()<stream_detail::WindowSlot>(l.window_pages);
    w.history_ = array.template operator()<stream_detail::HistoryVersion>(l.history_versions);
    w.hash_ = array.template operator()<u32>(2 * l.history_versions + 1);
    auto roots = PrepareNativeCatalogRootsRead(l.maximum_profile, memory, binding);
    if (!roots.ok()) { out.error = E::memory_failure; out.memory_error = roots.memory_error; return out; }
    w.roots_ = std::move(roots.workspace);
    const auto slots = w.arena_.Allocate(l.window_pages * sizeof(NativeCatalogLeafPreparedMemory), alignof(NativeCatalogLeafPreparedMemory));
    if (!slots.ok()) throw std::bad_alloc();
    auto* leaf_slots = static_cast<NativeCatalogLeafPreparedMemory*>(slots.pointer);
    std::uninitialized_value_construct_n(leaf_slots, l.window_pages);
    w.leaves_ = {leaf_slots, l.window_pages};
    for (auto& slot : w.leaves_) {
      auto leaf = PrepareNativeCatalogLeafRead(l.maximum_profile, memory, binding, NativeCatalogLeafProfileMode::maximum);
      if (!leaf.ok()) { out.error = E::memory_failure; out.memory_error = leaf.memory_error; return out; }
      slot = std::move(leaf.workspace);
    }
    w.limits_ = l; w.workspace_bytes_ = total; w.largest_ = largest;
    out.workspace = std::move(w);
    out.error = E::none;
  } catch (const std::bad_alloc&) { out.error = E::resource_exhausted; }
    catch (const std::length_error&) { out.error = E::resource_exhausted; }
  return out;
}

// SB_OPEN_COHORT_STREAM (2026-11-27): hooks for a caller that must verify more than the visible rows (the open's cohort
// scan). on_leaf runs once per physical base-leaf read (a leaf the window evicted and read again runs it again, so a hook
// keeps its own per-page registry), with EVERY stored row of the page in view; on_tree_page runs once per index page after
// its topology is validated; kAllHistoryGroups makes the end-of-pass chain checks run for every row group that has history,
// not only the groups a cell reached (visibility is still judged for reached groups only). A hook returns false to refuse
// (hook_refused); it keeps its own detail. The default hooks do nothing and cost nothing.
struct NativeCatalogStreamNoHooks {
  static constexpr bool kActive = false;
  static constexpr bool kAllHistoryGroups = false;
  bool on_leaf(const disk::NativePageReference&, const NativeCatalogLeafPageView&) noexcept { return true; }
  bool on_tree_page(const disk::NativePageReference&, const page::NativeBtreePageView&, bool) noexcept { return true; }
};
// SB_STREAM_PASS_ANCHOR (2026-11-27) TEST SEAM: called after every base-leaf read of a pass with the number of leaf reads so far.
// Null in production. It is a plain function pointer (not macro-guarded) because this header is instantiated in libraries and in
// test executables alike and one definition must serve all of them. A test uses it to change page zero between two leaf reads.
inline void (*g_native_stream_leaf_read_test_seam)(u64 leaf_reads) noexcept = nullptr;
// A tree walked on its own (a page-zero tree: the name trees and the comment tree): the root, the index it must belong
// to, and the callback for every leaf cell. No base leaf is read, no visibility is judged.
struct NativeCatalogStreamTreeOnly {
  disk::NativePageReference root;
  Uuid index_uuid;
};
struct NativeCatalogStreamResult {
  NativeCatalogStreamError error = NativeCatalogStreamError::invalid_request;
  NativeCatalogRootsLeaseError roots_error = NativeCatalogRootsLeaseError::none;
  page::NativeBtreeError tree_error = page::NativeBtreeError::none;
  NativeCatalogLeafMemoryError leaf_error = NativeCatalogLeafMemoryError::none;
  disk::BoundedIoResult io;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  Uuid relation_uuid;  // the relation's identity as the first base leaf (or an unindexed head) gave it; nil for an empty indexed relation
  bool indexed = false;  // the relation's head is an index (page type 0x200), not one leaf
  Uuid head_object_uuid;  // the head's object: the index UUID, or for an unindexed relation the relation's own
  bool stopped = false;  // the visitor returned false
  // Counters of the pass (for the report and the tests, not a limit).
  u64 rows_visited = 0,       // visible rows handed to the visitor
      versions_examined = 0,  // versions judged
      cells = 0,              // index cells walked
      leaf_reads = 0,         // base-leaf reads (a leaf evicted from the window and needed again is read again)
      tree_page_reads = 0, physical_bytes_read = 0, history_versions = 0, history_rows = 0,
      roots_retained_image_bytes = 0;  // the roots read's retained page-image bytes (a relation's share of the verified-image account)
  usize workspace_bytes = 0;
  bool ok() const noexcept { return error == NativeCatalogStreamError::none; }
};

class NativeCatalogVisibilityStreamReader {
 public:
  // The committed state at read time, or (At) the snapshot value captured earlier; the same decision function and the
  // same inputs as ReadCommittedSelected / ReadCommittedSelectedAt. `Visitor` is `bool(const NativeCatalogVisibleRow&)`:
  // the row (its metadata and name views) is valid only during the call; false stops the pass cleanly.
  template <class Lease, class Visitor>
  static NativeCatalogStreamResult StreamCommittedSelected(const Lease& source, u16 selector, u16 role, u64 image_budget,
                                                          NativeCatalogStreamPreparedMemory& w, Visitor&& visitor) noexcept {
    NativeCatalogStreamNoHooks none;
    return Run(detail::NativeLeaseSourceAccess::From(source), selector, role, nullptr, image_budget, w, visitor, none, nullptr);
  }
  // SB_OPEN_COHORT_STREAM: the same pass with hooks (see NativeCatalogStreamNoHooks).
  template <class Lease, class Visitor, class Hooks>
  static NativeCatalogStreamResult StreamCommittedSelectedHooked(const Lease& source, u16 selector, u16 role, u64 image_budget,
                                                                NativeCatalogStreamPreparedMemory& w, Visitor&& visitor, Hooks& hooks) noexcept {
    return Run(detail::NativeLeaseSourceAccess::From(source), selector, role, nullptr, image_budget, w, visitor, hooks, nullptr);
  }
  // A page-zero tree on its own: `on_cell` is `bool(const page::NativeBtreeCellView&)` for every leaf cell, in key order (false
  // stops the pass: `stopped`). The tree is read as a relation's head is (topology, fences, siblings, dependency tuple adopted
  // from the root, which must carry `index_uuid`); navigation-page creators are not judged (a name tree's pages are not rows).
  template <class Lease, class CellFn, class Hooks>
  static NativeCatalogStreamResult StreamTree(const Lease& source, const NativeCatalogStreamTreeOnly& tree, u64 image_budget,
                                              NativeCatalogStreamPreparedMemory& w, CellFn&& on_cell, Hooks& hooks) noexcept {
    return Run(detail::NativeLeaseSourceAccess::From(source), 0, 0, nullptr, image_budget, w, on_cell, hooks, &tree);
  }
  template <class Lease, class Visitor>
  static NativeCatalogStreamResult StreamCommittedSelectedAt(const Lease& source, const transaction::mga::VisibilitySnapshot& view,
                                                            u16 selector, u16 role, u64 image_budget,
                                                            NativeCatalogStreamPreparedMemory& w, Visitor&& visitor) noexcept {
    const auto borrowed = transaction::mga::BorrowVisibilitySnapshot(view);
    NativeCatalogStreamNoHooks none;
    return Run(detail::NativeLeaseSourceAccess::From(source), selector, role, &borrowed, image_budget, w, visitor, none, nullptr);
  }

 private:
  using Slot = stream_detail::WindowSlot;
  template <class Visitor, class Hooks>
  static NativeCatalogStreamResult Run(const detail::NativeLeaseReadSource& source, u16 selector, u16 role,
                                       const transaction::mga::VisibilitySnapshotView* point, u64 image_budget,
                                       NativeCatalogStreamPreparedMemory& w, Visitor& visitor, Hooks& hooks,
                                       const NativeCatalogStreamTreeOnly* tree_only) noexcept {
    namespace mga = transaction::mga;
    namespace catalog = core::catalog;
    using E = NativeCatalogStreamError;
    using TE = page::NativeBtreeError;
    NativeCatalogStreamResult out;
    out.workspace_bytes = w.workspace_bytes_;
    constexpr bool kCellVisitor = std::is_invocable_r_v<bool, Visitor&, const page::NativeBtreeCellView&>;
    try {
      if (!w || (!tree_only && ((selector != 2 && selector != 8) || role < 1 || role > page::kNativeCatalogRelationRoles))) return out;
      if (kCellVisitor != (tree_only != nullptr)) return out;
      if (source.database_uuid.is_nil() || w.arena_.binding().database_uuid != source.database_uuid || !source.inventory) return out;
      if (source.cluster_authority_required) { out.error = E::cluster_requires_authority; return out; }
      const auto& inventory = *source.inventory;
      const auto& inventory_entries = inventory.entries;
      const auto snapshot = NativeCatalogVisibilityLeaseReader::CommittedSnapshotOf(inventory, point);
      constexpr auto absent = std::numeric_limits<usize>::max();
      const auto fail = [&](E e) { out.error = e; return out; };
      out.error = E::none;  // every refusal below sets its own

      // ---- roots and the relation's head ----
      u64 retained = source.retained_image_bytes;
      if (retained >= image_budget) return fail(E::resource_exhausted);
      disk::NativePageReference head_page;
      Uuid selected_index;
      bool indexed = true;
      if (tree_only) {
        head_page = tree_only->root;
        selected_index = tree_only->index_uuid;
        if (!head_page.page_number || !head_page.page_generation || !core::uuid::IsEngineIdentityUuid(head_page.filespace_uuid) ||
            !disk::FindCanonicalFilespacePageProfile(head_page.page_size_profile_uuid) || !core::uuid::IsEngineIdentityUuid(selected_index))
          return out;
      } else {
        const auto roots = NativeCatalogRootsLeaseReader::ReadWith(source, image_budget - retained, w.roots_);
        out.physical_bytes_read += roots.physical_bytes_read;
        out.roots_retained_image_bytes = roots.retained_image_bytes;
        out.roots_error = roots.error;
        if (!roots.ok()) return fail(E::root_failure);
        const auto& catalog_root = roots.catalogs[selector == 2 ? 0 : roots.feature_root_index].root;
        const auto head = std::find_if(catalog_root.roots.begin(), catalog_root.roots.end(), [&](const auto& r) { return r.role == role; });
        if (head == catalog_root.roots.end()) return fail(E::missing_relation);
        indexed = head->page_type == 0x200;
        if (!indexed && head->page_type != 6) return fail(E::binding_mismatch);
        head_page = head->page;
        selected_index = head->object_uuid;
        out.indexed = indexed;
        out.head_object_uuid = head->object_uuid;
      }

      // ---- creators (as the collect-everything reader resolves them) ----
      const auto creator = [&](u64 number, const Uuid& uuid) -> usize {
        const auto entry = std::lower_bound(inventory_entries.begin(), inventory_entries.end(), number,
                                            [](const auto& r, u64 n) { return r.identity.local_id.value < n; });
        if (entry == inventory_entries.end() || entry->identity.local_id.value != number || entry->identity.transaction_uuid.value != uuid) {
          out.error = E::creator_mismatch; return absent;
        }
        if (entry->identity.scope != mga::TransactionScope::local_node) { out.error = E::cluster_requires_authority; return absent; }
        return static_cast<usize>(entry - inventory_entries.begin());
      };

      // ---- the window of base leaves ----
      bool anchored = false;  // set once every file's page zero is verified at the pass start (a cursor over an index)
      // SB_STREAM_PASS_ANCHOR: once per pass, after the last leaf: page zero of every file is read again and must equal the image
      // the pass verified at its start byte for byte, and the file's size must be what it was. (Per leaf the pass keeps the
      // size check and the page's own header checks; the bootstrap, page-zero decode and page-zero compare run per pass.)
      const auto verify_zero = [&]() -> bool {
        if (!anchored) return true;
        for (usize i = 0; i < source.devices.size(); ++i) {
          const auto& f = w.files_[i];
          auto io = source.batches[i].ReadAtBounded(0, w.zero_check_.data(), f.size);
          out.physical_bytes_read += io.bytes_transferred;
          out.io = io;
          if (!io.ok() || io.bytes_transferred != f.size) { out.error = E::io_failure; return false; }
          if (!std::equal(w.zero_check_.begin(), w.zero_check_.begin() + f.size, w.zero_.begin() + i * w.largest_)) {
            out.leaf_error = NativeCatalogLeafMemoryError::page_zero_failure; out.error = E::leaf_failure; return false;
          }
          const auto size = source.batches[i].SizeBounded();
          out.io = size;
          if (!size.ok()) { out.error = E::io_failure; return false; }
          if (size.size_bytes != f.total_pages * f.size) { out.leaf_error = NativeCatalogLeafMemoryError::page_zero_failure; out.error = E::leaf_failure; return false; }
        }
        return true;
      };
      Uuid relation = {};
      // An unindexed relation's head IS its one leaf, and its object is the relation (as the collect-everything reader binds it).
      if (!tree_only && !indexed) relation = out.head_object_uuid;
      u64 tick = 0;
      for (auto& s : w.window_) { s = {}; }
      const auto window_acquire = [&](const disk::NativePageReference& ref) -> Slot* {
        for (auto& s : w.window_)
          if (s.valid && s.ref == ref) { s.tick = ++tick; return &s; }
        usize victim = absent;
        for (usize i = 0; i < w.window_.size(); ++i) {
          const auto& s = w.window_[i];
          if (s.pins) continue;
          if (!s.valid) { victim = i; break; }
          if (victim == absent || s.tick < w.window_[victim].tick) victim = i;
        }
        if (victim == absent) { out.error = E::resource_exhausted; return nullptr; }
        auto& s = w.window_[victim];
        s.valid = false; s.scanned = false;
        const page::NativeCatalogRootReference target{role, 6, ref, relation};
        NativeCatalogLeafLeaseReadResult leaf;
        if (anchored) {
          // Page zero was verified at the pass start and is compared again after the last leaf (see PassAnchor).
          const auto f = std::find_if(w.files_.begin(), w.files_.begin() + static_cast<std::ptrdiff_t>(source.devices.size()),
                                      [&](const auto& x) { return x.source.filespace_uuid == ref.filespace_uuid; });
          if (f == w.files_.begin() + static_cast<std::ptrdiff_t>(source.devices.size())) { out.leaf_error = NativeCatalogLeafMemoryError::binding_mismatch; out.error = E::leaf_failure; return nullptr; }
          const NativeCatalogLeafLeaseReader::PassAnchor anchor{f->total_pages, f->size, f->flags, f->role};
          leaf = NativeCatalogLeafLeaseReader::ReadAnchored(source, target, w.leaves_[victim], anchor, relation.is_nil());
        } else {
          leaf = relation.is_nil() ? NativeCatalogLeafLeaseReader::ReadSelected(source, target, w.leaves_[victim])
                                   : NativeCatalogLeafLeaseReader::Read(source, target, w.leaves_[victim]);
        }
        out.physical_bytes_read += leaf.bytes_read;
        ++out.leaf_reads;
        if (g_native_stream_leaf_read_test_seam) g_native_stream_leaf_read_test_seam(out.leaf_reads);
        if (!leaf.ok()) { out.leaf_error = leaf.error; out.io = leaf.io; out.error = E::leaf_failure; return nullptr; }
        if (relation.is_nil()) relation = leaf.leaf.page->body.relation_uuid.value;
        out.relation_uuid = relation;
        s.page = *leaf.leaf.page; s.ref = ref; s.valid = true; s.tick = ++tick;
        if constexpr (Hooks::kActive) {
          if (!hooks.on_leaf(ref, s.page)) { s.valid = false; out.error = E::hook_refused; return nullptr; }
        }
        return &s;
      };
      struct Pin {
        Slot* s;
        explicit Pin(Slot* slot) : s(slot) { if (s) ++s->pins; }
        ~Pin() { if (s) --s->pins; }
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
      };

      // ---- the history registry (rows with more than one version) ----
      usize history_count = 0;
      std::fill(w.hash_.begin(), w.hash_.end(), std::numeric_limits<u32>::max());
      const auto find_history = [&](const Uuid& version) -> usize {
        auto slot = btree_memory_detail::Hash(version) % w.hash_.size();
        while (w.hash_[slot] != std::numeric_limits<u32>::max()) {
          if (w.history_[w.hash_[slot]].version_uuid == version) return w.hash_[slot];
          if (++slot == w.hash_.size()) slot = 0;
        }
        return absent;
      };
      const auto register_history = [&](const page::RowDataRecordView& r, const disk::NativePageReference& ref) -> bool {
        if (find_history(r.version_uuid) != absent) return true;
        if (history_count == w.history_.size()) { out.error = E::history_exhausted; return false; }
        auto slot = btree_memory_detail::Hash(r.version_uuid) % w.hash_.size();
        while (w.hash_[slot] != std::numeric_limits<u32>::max()) if (++slot == w.hash_.size()) slot = 0;
        w.hash_[slot] = static_cast<u32>(history_count);
        w.history_[history_count++] = {r.row_uuid.value, r.version_uuid, r.row_version, ref, false};
        out.history_versions = history_count;
        return true;
      };
      const auto has_history = [](const page::RowDataRecordView& r) {
        return r.previous_row_version != 0 || r.next_row_version != 0 || !r.next_version_uuid.is_nil() || !r.previous_version_uuid.is_nil();
      };
      // A page's rows that belong to a multi-version row are registered once per residency in the window.
      const auto scan = [&](Slot& s) -> bool {
        if (s.scanned) return true;
        for (const auto& r : s.page.body.rows)
          if (has_history(r) && !register_history(r, s.ref)) return false;
        s.scanned = true;
        return true;
      };

      // ---- judging one version, and handing the visible one to the visitor ----
      const auto metadata_of = [](const Slot& s, const Uuid& version) -> const NativeCatalogLeafRecordView* {
        const auto& md = s.page.metadata;
        const auto found = std::lower_bound(md.begin(), md.end(), version, [](const auto& m, const Uuid& id) { return m.version_uuid < id; });
        return found == md.end() || found->version_uuid != version ? nullptr : &*found;
      };
      // Judge a version against the snapshot: false on an error (out.error set), `visible` set, `decision` returned.
      const auto judge = [&](const Slot& s, const NativeCatalogLeafRecordView& m, mga::VisibilityDecision& decision) -> bool {
        const auto& r = s.page.body.rows[m.row_index];
        if (m.metadata.authority_scope == catalog::CatalogAuthorityScope::cluster) { out.error = E::cluster_requires_authority; return false; }
        const auto at = creator(r.local_transaction_id, r.transaction_uuid.value);
        if (at == absent) return false;
        const auto version = NativeCatalogVisibilityLeaseReader::VersionMetadataOf(r, inventory_entries[at]);
        const auto validated = mga::ObserveRowVersionMetadata(version);
        if (!validated.ok()) { out.visibility_diagnostic = validated; out.error = E::visibility_failure; return false; }
        const auto d = mga::ObserveVersionEffectVisibility(version, snapshot);
        if (d.decision == mga::VisibilityDecision::requires_recovery) { out.visibility_diagnostic = d.outcome; out.error = E::requires_recovery; return false; }
        if (!d.ok() && d.decision != mga::VisibilityDecision::wait_for_transaction) { out.visibility_diagnostic = d.outcome; out.error = E::visibility_failure; return false; }
        ++out.versions_examined;
        decision = d.decision;
        return true;
      };
      // Hand a visible version over. Returns false to end the pass (error set, or the visitor stopped).
      const auto emit = [&](const Slot& s, const NativeCatalogLeafRecordView& m) -> bool {
        const auto& r = s.page.body.rows[m.row_index];
        const auto at = creator(r.local_transaction_id, r.transaction_uuid.value);
        if (at == absent) return false;
        NativeCatalogVisibleRow value{m.metadata, r.version_uuid, r.previous_version_uuid, {},
                                      !mga::HasCommittedInventoryOutcome(inventory_entries[at]), m.metadata.lifecycle, m.metadata.status,
                                      r.row_uuid.value};
        if (m.metadata.record.header.kind == catalog::CatalogRecordKind::localized_name) {
          const auto decoded = NativeCatalogVisibilityLeaseReader::NameOf(s.page, r, m.metadata);
          if (!decoded.ok()) { out.error = E::name_payload_invalid; return false; }
          value.name_payload = decoded.record->payload;
        }
        if (value.provisional) {
          value.effective_status = catalog::CatalogObjectStatus::proposed;
          value.effective_lifecycle = m.metadata.record.header.deleted ? catalog::CatalogObjectLifecycle::dropping
                                      : m.metadata.definition_version == 1 ? catalog::CatalogObjectLifecycle::creating
                                                                           : catalog::CatalogObjectLifecycle::altering;
        }
        ++out.rows_visited;
        if constexpr (kCellVisitor) {
          return false;  // (a cell visitor never reaches emit: tree-only passes read no base leaf)
        } else {
          if (!visitor(static_cast<const NativeCatalogVisibleRow&>(value))) { out.stopped = true; return false; }
          return true;
        }
      };
      // A version that is the only one of its row: judged and handed over at once.
      const auto handle_single = [&](Slot& s, const NativeCatalogLeafRecordView& m) -> bool {
        mga::VisibilityDecision d{};
        if (!judge(s, m, d)) return false;
        return d != mga::VisibilityDecision::visible || emit(s, m);
      };

      // ---- the pass: single-version rows now, history rows registered for the end ----
      bool go = true;
      const auto handle_version = [&](Slot& s, const NativeCatalogLeafRecordView& m, const page::NativeBtreeCellView* cell) -> bool {
        const auto& r = s.page.body.rows[m.row_index];
        if (cell) {
          // The cell's creator is the version's (SB_NATIVE_BTREE_COMPRESSED_PAGE, owner Q38 = 1); the row is the cell's.
          if (r.row_uuid.value != cell->key.row_uuid) { out.error = E::binding_mismatch; return false; }
          if (r.local_transaction_id != cell->creator_local_transaction_id) { out.error = E::binding_mismatch; return false; }
        }
        if (!has_history(r)) return handle_single(s, m);
        const auto at = find_history(r.version_uuid);
        if (at == absent) { out.error = E::invalid_chain; return false; }  // registered by the page's scan: cannot be absent
        w.history_[at].candidate = true;
        return true;
      };
      if (indexed) {
        // The cursor: root to leaf, left to right, two pages per level.
        const auto files_count = source.batch_count;
        if (source.devices.empty() || source.devices.size() != files_count || source.devices.size() > w.files_.size()) { out.error = E::invalid_request; return out; }
        auto files = w.files_.first(source.devices.size());
        const auto read = [&](usize i, u64 offset, void* data, usize size) {
          auto io = source.batches[i].ReadAtBounded(offset, data, size);
          out.physical_bytes_read += io.bytes_transferred;
          out.io = io;
          return io.ok() && io.bytes_transferred == size;
        };
        for (usize i = 0; i < files.size(); ++i) {
          const auto& f = source.devices[i];
          const auto* p = disk::FindCanonicalFilespacePageProfile(f.page_size_profile_uuid);
          if (!f.device || !p || !core::uuid::IsEngineIdentityUuid(f.filespace_uuid) || (i && !(source.devices[i - 1].filespace_uuid < f.filespace_uuid))) {
            out.tree_error = TE::invalid_filespace; return fail(E::tree_failure);
          }
          if (p->page_size_bytes > w.largest_) { out.tree_error = TE::resource_exhausted; return fail(E::tree_failure); }
          files[i] = {f};
          const disk::FilespaceBootstrapBinding expected{source.database_uuid, f.filespace_uuid, f.page_size_profile_uuid};
          disk::SerializedFilespaceBootstrap prefix{};
          if (!read(i, 0, prefix.data(), prefix.size())) { out.tree_error = TE::io_failure; return fail(E::io_failure); }
          const auto bootstrap = disk::DecodeFilespaceBootstrap(prefix.data(), prefix.size(), &expected);
          if (!bootstrap.ok()) { out.tree_error = TE::invalid_filespace; return fail(E::tree_failure); }
          files[i].size = bootstrap.preamble->page_size_bytes;
          if (i >= w.zero_.size() / w.largest_) { out.tree_error = TE::resource_exhausted; return fail(E::tree_failure); }
          byte* zero_image = w.zero_.data() + i * w.largest_;
          if (!read(i, 0, zero_image, files[i].size)) { out.tree_error = TE::io_failure; return fail(E::io_failure); }
          const std::span<const byte> image{zero_image, files[i].size};
          if (!std::equal(prefix.begin(), prefix.end(), image.begin())) { out.tree_error = TE::invalid_filespace; return fail(E::tree_failure); }
          const auto zero = disk::DecodeFilespacePageZeroInto(image, w.root_refs_, &expected);
          if (!zero.ok()) { out.tree_error = TE::invalid_filespace; return fail(E::tree_failure); }
          files[i].total_pages = zero.record->total_pages;
          files[i].role = zero.record->bootstrap.filespace_role;
          files[i].flags = zero.record->bootstrap.flags;
          const auto size = source.batches[i].SizeBounded();
          out.io = size;
          if (!size.ok()) { out.tree_error = TE::io_failure; return fail(E::io_failure); }
          if (files[i].total_pages > std::numeric_limits<u64>::max() / files[i].size || size.size_bytes != files[i].total_pages * files[i].size) {
            out.tree_error = TE::invalid_filespace; return fail(E::tree_failure);
          }
        }
        anchored = true;
        const auto filespace = [&](const disk::NativePageReference& ref) {
          const auto f = std::lower_bound(files.begin(), files.end(), ref.filespace_uuid, [](const auto& a, const Uuid& id) { return a.source.filespace_uuid < id; });
          return f == files.end() || f->source.filespace_uuid != ref.filespace_uuid || f->source.page_size_profile_uuid != ref.page_size_profile_uuid ||
                         ref.page_number >= f->total_pages ? files.size() : static_cast<usize>(f - files.begin());
        };
        constexpr usize depth = kNativeCatalogStreamTreeDepth;
        stream_detail::StreamTreeSlot* current[depth + 1] = {};
        bool unlisted_at[depth + 1] = {};
        auto& root_slot = w.slots_[2 * depth];
        auto pending = w.pending_;
        usize pending_count = 1;
        pending[0] = {};
        pending[0].ref = head_page;
        pending[0].root = true;
        page::NativeBtreeDependencies dependencies;
        while (pending_count) {
          const auto next = pending[--pending_count];
          const auto fs = filespace(next.ref);
          if (fs == files.size()) { out.tree_error = TE::invalid_filespace; return fail(E::tree_failure); }
          const auto& f = files[fs];
          if (f.flags & disk::FilespaceBootstrapFlag::payload_encrypted) { out.tree_error = TE::encrypted_requires_crypto_authority; return fail(E::tree_failure); }
          if (f.flags & disk::FilespaceBootstrapFlag::cluster_authority_required) { out.tree_error = TE::cluster_requires_authority; return fail(E::cluster_requires_authority); }
          // The slot this page decodes into: the root's own, or the level's idle one of its two.
          stream_detail::StreamTreeSlot* slot = nullptr;
          if (next.root) slot = &root_slot;
          else {
            if (next.level >= depth) { out.tree_error = TE::resource_exhausted; return fail(E::resource_exhausted); }
            auto* a = &w.slots_[2 * next.level];
            auto* b = &w.slots_[2 * next.level + 1];
            slot = current[next.level] == a ? b : a;
          }
          if (f.size > slot->image.size()) { out.tree_error = TE::resource_exhausted; return fail(E::resource_exhausted); }
          auto* image = slot->image.data();
          if (!read(fs, next.ref.page_number * f.size, image, f.size)) { out.tree_error = TE::io_failure; return fail(E::io_failure); }
          ++out.tree_page_reads;
          const u32 type = next.root ? 0x200 : next.level ? 0x201 : 0x202;
          const disk::NativeCommonPageHeaderBinding expected{{source.database_uuid, next.ref.filespace_uuid, next.ref.page_size_profile_uuid},
                                                             next.ref.page_number, next.ref.page_generation, type, {}};
          const auto common = disk::DecodeNativeCommonPageHeader(image, 128, &expected);
          if (!common.ok()) { out.tree_error = common.error == disk::NativeCommonPageHeaderError::binding_mismatch ? TE::binding_mismatch : TE::invalid_header; return fail(E::tree_failure); }
          if (common.header->flags & 1u) { out.tree_error = TE::encrypted_requires_crypto_authority; return fail(E::tree_failure); }
          if (common.header->flags & 2u) { out.tree_error = TE::cluster_requires_authority; return fail(E::cluster_requires_authority); }
          if (common.header->flags & 12u) { out.tree_error = TE::header_policy_requires_authority; return fail(E::tree_failure); }
          slot->page = page::DecodeNativeBtreePageInto({image, f.size}, {slot->cells, w.references_, slot->keys});
          if (!slot->page.ok()) { out.tree_error = slot->page.error; return fail(E::tree_failure); }
          const auto& p = *slot->page.page;
          if (next.root) {
            if (p.dependencies.index_uuid != selected_index) { out.tree_error = TE::binding_mismatch; return fail(E::tree_failure); }
            dependencies = p.dependencies;
            if (p.tree_level >= depth) { out.tree_error = TE::resource_exhausted; return fail(E::resource_exhausted); }
          }
          if (p.dependencies != dependencies) { out.tree_error = TE::binding_mismatch; return fail(E::tree_failure); }
          const auto in_range = [&](const auto& r) { return !r || r->filespace_uuid != next.ref.filespace_uuid || r->page_number < f.total_pages; };
          if (!in_range(p.parent) || !in_range(p.left) || !in_range(p.right) || !in_range(p.first_child)) { out.tree_error = TE::invalid_reference; return fail(E::tree_failure); }
          for (const auto& c : p.cells) if (!in_range(c.child) || !in_range(c.base_page)) { out.tree_error = TE::invalid_reference; return fail(E::tree_failure); }
          const auto level = p.tree_level;
          if (!next.root && level != next.level) { out.tree_error = TE::tree_level_mismatch; return fail(E::tree_failure); }
          // Navigation pages must have a committed creator (structure, not data), judged as the collect-everything reader does.
          if (!tree_only && !(p.creator_transaction_uuid.is_nil() && !p.creator_local_transaction_id)) {
            const auto at = creator(p.creator_local_transaction_id, p.creator_transaction_uuid);
            if (at == absent) return out;
            if (!mga::HasCommittedInventoryOutcome(inventory_entries[at])) return fail(E::navigation_not_committed);
          }
          const auto* parent = next.root ? nullptr : current[level + 1];
          if (!next.root && !parent) { out.tree_error = TE::tree_reference_mismatch; return fail(E::tree_failure); }
          const auto* prior = current[level];
          const bool lags = parent && page::detail::NativeBtreeSplitLags(p, *parent->page.page, next.high);
          const auto topology = page::detail::ValidateNativeBtreeTraversalStep(
              p, parent ? &*parent->page.page : nullptr, next.level, next.low, lags ? p.high_fence : next.high,
              prior ? &*prior->page.page : nullptr, prior && unlisted_at[level]);
          if (topology != TE::none) { out.tree_error = next.unlisted ? TE::tree_fence_mismatch : topology; return fail(E::tree_failure); }
          for (const auto& cell : p.cells)
            if (cell.base_page && filespace(*cell.base_page) == files.size()) { out.tree_error = TE::invalid_filespace; return fail(E::tree_failure); }
          if constexpr (Hooks::kActive) {
            if (!hooks.on_tree_page(next.ref, p, next.root)) { out.error = E::hook_refused; return out; }
          }
          current[level] = slot;
          unlisted_at[level] = next.unlisted;
          if (lags) {
            if (pending_count == pending.size()) return fail(E::resource_exhausted);
            stream_detail::StreamPending sibling;
            sibling.ref = *p.right; sibling.level = next.level; sibling.low = p.high_fence; sibling.high = next.high; sibling.unlisted = true;
            pending[pending_count++] = sibling;
          }
          if (!level) {
            // A leaf: its cells, each bound to a row version on a base leaf.
            if constexpr (kCellVisitor) {
              for (const auto& cell : p.cells) {
                ++out.cells;
                if (!visitor(cell)) { out.stopped = true; go = false; break; }
              }
              if (!go) break;
              continue;
            }
            for (const auto& cell : p.cells) {
              ++out.cells;
              if (!cell.base_page) return fail(E::invalid_locator);
              Slot* s = window_acquire(*cell.base_page);
              if (!s) return out;
              Pin pin(s);
              if (s->ref.page_generation != cell.base_page->page_generation) return fail(E::binding_mismatch);
              if (!scan(*s)) return out;
              const auto* m = metadata_of(*s, cell.key.version_uuid);
              if (!m) return fail(E::invalid_locator);
              if (!handle_version(*s, *m, &cell)) { go = false; break; }
            }
            if (!go) break;
            continue;
          }
          if (p.cells.size() + 1 > pending.size() - pending_count) return fail(E::resource_exhausted);
          const auto child_level = static_cast<u16>(level - 1);
          for (usize i = p.cells.size(); i > 0; --i) {
            const auto& c = p.cells[i - 1];
            stream_detail::StreamPending child;
            child.ref = *c.child; child.level = child_level; child.low = c.key;
            child.high = i == p.cells.size() ? p.high_fence : std::optional{p.cells[i].key};
            pending[pending_count++] = child;
          }
          stream_detail::StreamPending first;
          first.ref = *p.first_child; first.level = child_level; first.low = p.low_fence; first.high = p.cells.front().key;
          pending[pending_count++] = first;
        }
        if (go) {
          // The last page at every level has no right sibling.
          for (usize l = 0; l <= depth; ++l)
            if (current[l] && current[l]->page.page->right) { out.tree_error = TE::tree_sibling_mismatch; return fail(E::tree_failure); }
        }
      } else {
        // An unindexed relation is one leaf; every row on it is a candidate.
        Slot* s = window_acquire(head_page);
        if (!s) return out;
        Pin pin(s);
        if (!scan(*s)) return out;
        for (const auto& m : s->page.metadata)
          if (!handle_version(*s, m, nullptr)) { go = false; break; }
      }
      if (!go) {
        if (out.stopped) { out.error = E::none; if (!verify_zero()) return out; }
        return out;
      }
      if (tree_only) { out.error = E::none; if (!verify_zero()) return out; return out; }

      // ---- rows with history: judged now that every page has passed ----
      auto history = w.history_.first(history_count);
      std::sort(history.begin(), history.end(), [](const auto& a, const auto& b) {
        return a.row_uuid == b.row_uuid ? a.row_version > b.row_version : a.row_uuid < b.row_uuid;
      });
      for (usize begin = 0; begin < history.size();) {
        usize end = begin + 1;
        bool candidate = history[begin].candidate;
        while (end < history.size() && history[end].row_uuid == history[begin].row_uuid) candidate |= history[end++].candidate;
        const auto group_begin = begin;
        begin = end;
        if (!candidate && !Hooks::kAllHistoryGroups) continue;
        if (candidate) ++out.history_rows;
        // The chain among a group's versions: each version's previous (when it names one) is in the group and agrees.
        for (usize n = group_begin; n < end; ++n) {
          Slot* s = window_acquire(history[n].page);
          if (!s) return out;
          Pin pin(s);
          const auto* m = metadata_of(*s, history[n].version_uuid);
          if (!m) return fail(E::invalid_locator);
          const auto& r = s->page.body.rows[m->row_index];
          if (!r.previous_row_version) continue;
          usize p = absent;
          for (usize k = group_begin; k < end; ++k) if (history[k].version_uuid == r.previous_version_uuid) { p = k; break; }
          if (p == absent) continue;  // judged below (missing_version when the version is not the visible one)
          if (history[p].row_version != r.previous_row_version) return fail(E::invalid_chain);
          Slot* ps = window_acquire(history[p].page);
          if (!ps) return out;
          Pin pin2(ps);
          const auto* pm = metadata_of(*ps, history[p].version_uuid);
          if (!pm) return fail(E::invalid_locator);
          const auto& before = pm->metadata;
          const auto& after = m->metadata;
          if (before.record.header.object_uuid.value != after.record.header.object_uuid.value || before.record.header.kind != after.record.header.kind) return fail(E::invalid_chain);
          if (after.record.header.kind == catalog::CatalogRecordKind::localized_name) {
            const auto a = NativeCatalogVisibilityLeaseReader::NameOf(ps->page, ps->page.body.rows[pm->row_index], before);
            const auto b = NativeCatalogVisibilityLeaseReader::NameOf(s->page, r, after);
            if (!a.ok() || !b.ok() || !catalog::CatalogNamePayloadPreservesIdentity(a.record->payload, b.record->payload)) return fail(E::invalid_chain);
          }
          if (!catalog::CatalogMetadataPreservesFamilyOrigin(before, after) || before.definition_version == std::numeric_limits<u64>::max() ||
              after.definition_version != before.definition_version + 1 || after.schema_epoch < before.schema_epoch ||
              after.security_epoch < before.security_epoch || after.resource_epoch < before.resource_epoch ||
              after.catalog_generation < before.catalog_generation || after.dependency_generation < before.dependency_generation ||
              after.invalidation_generation < before.invalidation_generation)
            return fail(E::invalid_chain);
        }
        if (!candidate) continue;  // (kAllHistoryGroups: the chain above is checked for every group; visibility only for reached ones)
        // Newest first: the first version the snapshot sees is the row's.
        for (usize n = group_begin; n < end; ++n) {
          Slot* s = window_acquire(history[n].page);
          if (!s) return out;
          Pin pin(s);
          const auto* m = metadata_of(*s, history[n].version_uuid);
          if (!m) return fail(E::invalid_locator);
          mga::VisibilityDecision d{};
          if (!judge(*s, *m, d)) return out;
          if (d != mga::VisibilityDecision::visible) {
            const auto& r = s->page.body.rows[m->row_index];
            if (r.previous_row_version) {
              bool found = false;
              for (usize k = group_begin; k < end && !found; ++k) found = history[k].version_uuid == r.previous_version_uuid;
              if (!found) return fail(E::missing_version);
            }
            continue;
          }
          if (!emit(*s, *m)) { if (out.stopped) { out.error = E::none; verify_zero(); } return out; }
          break;
        }
      }
      out.error = E::none;
      verify_zero();
      return out;
    } catch (const std::bad_alloc&) { out.error = E::resource_exhausted; }
      catch (const std::length_error&) { out.error = E::resource_exhausted; }
      catch (...) { out.error = E::io_failure; }
    return out;
  }
};
}  // namespace scratchbird::storage::database
