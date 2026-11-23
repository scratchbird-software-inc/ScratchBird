// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_catalog_version_writer.hpp"

#include <deque>

#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_identifier_profile.hpp"
#include "catalog_name_path.hpp"
#include "catalog_name_tree_keys.hpp"
#include "catalog_comment_record_codec.hpp"
#include "native_catalog_constant_relations.hpp"
#include "catalog_metadata_change.hpp"
#include "catalog_construction_intent.hpp"
#include "filespace_page_zero.hpp"
#include "allocation_map_writer.hpp"
#include "native_btree_tree_backing.hpp"
#include <array>
#include <functional>
#include <variant>
#include <tuple>
#include "catalog_naming_collision_policy.hpp"
#include "catalog_naming_dialect_profile.hpp"
#include "catalog_page.hpp"
#include "catalog_policy_descriptor.hpp"
#include "catalog_records.hpp"
#include "native_common_page_header.hpp"
#include "native_index_btree_page.hpp"
#include "transaction_inventory_reader.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <unordered_map>

namespace scratchbird::storage::database {
namespace {
namespace disk = scratchbird::storage::disk;
namespace pg = scratchbird::storage::page;
namespace catalog = scratchbird::core::catalog;
namespace mga = scratchbird::transaction::mga;
using E = CatalogVersionWriteError;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using disk::CarefulWriteError;

// SB_PART_B_S6_NAME_ADMISSION: the engine's one text rule for a name ENTRY's raw_name_text, as a writer refusal with its own
// code. Applied BEFORE the record codec's own validity check (CatalogNamePayloadMatchesMetadata) so that ill-formed UTF-8 and an
// over-long text get their specific codes rather than invalid_insert / invalid_successor. "" passes. A name vector carries no
// text. Used on inserts and on successors that are not drops (a drop re-states a name already stored).
std::optional<E> NameTextRefusal(const catalog::CatalogNamePayload& payload) {
  const auto* entry = std::get_if<catalog::CatalogNameEntry>(&payload);
  if (!entry) return std::nullopt;
  switch (catalog::AdmitCatalogIdentifierText(entry->raw_name_text)) {
    case catalog::CatalogIdentifierKeyError::none: return std::nullopt;
    case catalog::CatalogIdentifierKeyError::text_too_long: return E::name_too_long;
    case catalog::CatalogIdentifierKeyError::text_contains_nul: return E::name_contains_nul;
    case catalog::CatalogIdentifierKeyError::text_invalid_utf8: return E::name_invalid_utf8;
    case catalog::CatalogIdentifierKeyError::invalid_text: return E::name_null;
    default: return E::invalid_name;
  }
}

bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }

// The relations the startup verifiers place records in: the names relation
// (role 2) holds exactly the localized-name records (name verification);
// the comments relation (role 7) exactly the localized-comment records
// (comment verification, SB_NATIVE_COMMENT_RELATION); audit evidence is in
// role 6 (audit); database, schema and filespace records in role 1
// (namespaces, filespaces); inline private-key policies in role 3 (the create).
bool RoleAdmits(u16 role, const catalog::CatalogMetadataVersion& m) {
  using K = catalog::CatalogRecordKind;
  const auto kind = m.record.header.kind;
  if ((role == 2) != (kind == K::localized_name)) return false;
  if ((role == pg::kNativeCommentRelationRole) != (kind == K::localized_comment)) return false;
  if (kind == K::audit_evidence && role != 6) return false;
  if ((kind == K::database || kind == K::schema || kind == K::filespace) && role != 1) return false;
  if ((m.object_subtype == scratchbird::core::catalog::CatalogObjectSubtype::crypto_inline_private_key ||
       catalog::IsCatalogCryptoInlinePrivateKeyPolicyPayload(m.record.payload)) &&
      role != 3)
    return false;
  return true;
}
// Generations and epochs within the catalog root's. A name row's resource
// epoch is the resource epoch its keys were derived under (the name appendix's
// resource_epoch field; the create sets it from name_resource_epoch, 1), not
// the catalog's resource epoch, so it is not bounded by the root's: the create
// itself writes name entries at 1 under a root at 0. A definition's resource
// epoch is (SB_DRIFT_DEFINITION_RESOURCE_EPOCH). Owner ruling Q33 = 1
// (2026-11-09); spec annotation SB_CLARIFY_NAME_ROW_RESOURCE_EPOCH.
bool AheadOfRoot(const catalog::CatalogMetadataVersion& m, const pg::NativeCatalogRoot& r) {
  return m.catalog_generation > r.catalog_generation || m.schema_epoch > r.schema_epoch ||
         m.security_epoch > r.security_epoch ||
         (m.record.header.kind != catalog::CatalogRecordKind::localized_name && m.resource_epoch > r.resource_epoch);
}
// A name's object kind when it is a known kind (the codec admits no other;
// SB_CATALOG_NO_TEXT_IDENTIFIERS: the kind itself, not its name).
std::optional<catalog::CatalogRecordKind> KnownKind(catalog::CatalogRecordKind kind) {
  if (!catalog::IsKnownCatalogRecordKind(kind)) return std::nullopt;
  return kind;
}

// One base leaf as read: its image and its decode.
struct Leaf {
  std::vector<byte> image;
  NativeCatalogLeafPage page;
  std::map<Uuid, catalog::CatalogMetadataVersion> metadata;
};
// A version of the row: where it is.
struct Version {
  std::size_t leaf = 0, row = 0;
};

CatalogVersionWriteResult Refuse(E error) {
  CatalogVersionWriteResult r;
  r.error = error;
  return r;
}

bool ReadPage(disk::CarefulWriteSink& sink, u32 page_size, u64 number, std::vector<byte>& out,
              CatalogVersionWriteResult& result) {
  if (number == 0 || number > std::numeric_limits<u64>::max() / page_size) {
    result.error = E::invalid_catalog;
    return false;
  }
  out.assign(page_size, 0);
  const auto io = sink.ReadAt(number * page_size, std::span<byte>(out));
  if (!io.ok() || io.bytes_transferred != page_size) {
    result.error = E::io_failure;
    result.io = io;
    return false;
  }
  return true;
}

// The common header of an image read for `ref`, bound to it and to the
// database. page_type 0 accepts the B-tree family's three types.
bool HeaderBinds(const std::vector<byte>& image, const Uuid& database, const disk::NativePageReference& ref,
                 u32 page_type) {
  const auto h = disk::DecodeNativeCommonPageHeader(image.data(), std::min<std::size_t>(image.size(), 128));
  if (!h.ok()) return false;
  const auto& x = *h.header;
  return x.database_uuid == database && x.filespace_uuid == ref.filespace_uuid && x.page_number == ref.page_number &&
         x.page_generation == ref.page_generation && x.page_size_profile_uuid == ref.page_size_profile_uuid &&
         x.page_size_bytes == image.size() &&
         (page_type ? x.page_type == page_type : x.page_type >= 0x200 && x.page_type <= 0x202);
}

bool InProgress(mga::TransactionState state) {
  switch (state) {
    case mga::TransactionState::committed:
    case mga::TransactionState::rolled_back:
    case mga::TransactionState::failed_terminal: return false;
    default: return true;
  }
}

// The new metadata may follow the base (the same checks the visibility reader
// applies to a resident chain, and the creator the leaf codec binds).
bool FollowsAs(const catalog::CatalogMetadataVersion& before, const catalog::CatalogMetadataVersion& after, const Uuid& row,
               const Uuid& transaction_uuid, u64 transaction_order);
bool Follows(const catalog::CatalogMetadataVersion& before, const catalog::CatalogMetadataVersion& after,
             const CatalogVersionRequest& request) {
  return FollowsAs(before, after, request.row_uuid, request.transaction_uuid, request.transaction_order);
}
bool FollowsAs(const catalog::CatalogMetadataVersion& before, const catalog::CatalogMetadataVersion& after, const Uuid& row,
               const Uuid& transaction_uuid, u64 transaction_order) {
  const auto& b = before.record.header;
  const auto& a = after.record.header;
  if (catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(after))) return false;
  return a.kind == b.kind && a.object_uuid.kind == b.object_uuid.kind && a.object_uuid.value == b.object_uuid.value &&
         a.row_uuid.kind == UuidKind::row && a.row_uuid.value == row &&
         after.authority_scope != catalog::CatalogAuthorityScope::cluster &&
         after.creator_transaction_uuid.kind == UuidKind::transaction &&
         after.creator_transaction_uuid.value == transaction_uuid &&
         after.creator_local_transaction_id == transaction_order &&
         before.definition_version != std::numeric_limits<u64>::max() &&
         after.definition_version == before.definition_version + 1 && after.schema_epoch >= before.schema_epoch &&
         after.security_epoch >= before.security_epoch && after.resource_epoch >= before.resource_epoch &&
         after.catalog_generation >= before.catalog_generation &&
         after.dependency_generation >= before.dependency_generation &&
         after.invalidation_generation >= before.invalidation_generation &&
         catalog::CatalogMetadataPreservesFamilyOrigin(before, after);
}
// One index page as read: where, its image, its decode.
struct TreePage {
  disk::NativePageReference ref;
  std::vector<byte> image;
  pg::NativeBtreePage page;
  // SB_CATALOG_NEW_PAGES: a page this write adds (a split's new leaf); its
  // image is what the file holds there now, and it is written before the
  // pages that point to it.
  bool added = false;
  // SB_CATALOG_SPLIT_LAG (owner Q45 = 1): reached by a sibling link only, its
  // split interrupted before the parent's rewrite; the next write repairs it.
  bool unlisted = false;
  // The write order of a changed page (FirebirdSQL's precedence): 4 * level
  // + 0 for a page a split adds, + 1 for a split or otherwise changed page,
  // + 2 for an old right neighbour. Each rank is durable before the next.
  int rank = 1;
  // SB_CATALOG_PARENT_UNCHECKED (owner Q46 = 1): the page's parent, as the
  // walk reached it (an index into the tree); none for the root.
  std::optional<std::size_t> parent_index;
};
// A relation as its head reaches it: the index pages (empty when unindexed),
// in walk order, and every base leaf, sorted by page number.
struct Relation {
  std::vector<TreePage> tree;
  std::vector<disk::NativePageReference> bases;
  std::vector<Leaf> leaves;  // parallel to bases
};
}  // namespace

// SEARCH_KEY: SB_CATALOG_WRITER_PAGE_CACHE (owner Q190, Q191). Entries keyed
// by page number, each the image read and its decode (a leaf, without its
// image, or a B-tree page); `recent` orders them for eviction.
struct CatalogWriterPageCache::Impl {
  struct Entry {
    std::vector<byte> image;
    std::optional<Leaf> leaf;
    std::optional<pg::NativeBtreePage> tree;
    std::list<u64>::iterator recent;
  };
  explicit Impl(std::size_t maximum_pages) : maximum(std::max<std::size_t>(maximum_pages, 1)) {}
  // The entry for `number` when its image is `image`, made the most recent.
  Entry* Find(u64 number, const std::vector<byte>& image) {
    const auto at = entries.find(number);
    if (at == entries.end() || at->second.image != image) return nullptr;
    recent.splice(recent.begin(), recent, at->second.recent);
    return &at->second;
  }
  // A new entry for `number` holding `image` (any older one replaced), the
  // least recently used dropped while full.
  Entry& Put(u64 number, const std::vector<byte>& image) {
    if (const auto at = entries.find(number); at != entries.end()) {
      recent.erase(at->second.recent);
      entries.erase(at);
    }
    while (!recent.empty() && entries.size() >= maximum) {
      entries.erase(recent.back());
      recent.pop_back();
    }
    recent.push_front(number);
    auto& entry = entries[number];
    entry.image = image;
    entry.recent = recent.begin();
    return entry;
  }
  std::mutex mutex;
  std::size_t maximum = 1, hits = 0, misses = 0;
  std::unordered_map<u64, Entry> entries;
  std::list<u64> recent;
};

CatalogWriterPageCache::CatalogWriterPageCache(std::size_t maximum_pages)
    : impl_(std::make_unique<Impl>(maximum_pages)) {}
CatalogWriterPageCache::~CatalogWriterPageCache() = default;
std::size_t CatalogWriterPageCache::size() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->entries.size();
}
std::size_t CatalogWriterPageCache::maximum_pages() const noexcept { return impl_->maximum; }
std::size_t CatalogWriterPageCache::hits() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->hits;
}
std::size_t CatalogWriterPageCache::misses() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->misses;
}
void CatalogWriterPageCache::Clear() noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->entries.clear();
  impl_->recent.clear();
}

namespace {
// SB_CATALOG_WRITER_PAGE_CACHE: the decode of the leaf whose image was just
// read into `leaf.image` at `number`, when the binding's cache holds that very
// image; false (decode it) otherwise.
bool CachedLeaf(const CatalogVersionWriterBinding& binding, u64 number, Leaf& leaf) {
  if (!binding.page_cache) return false;
  auto& cache = binding.page_cache->impl();
  std::lock_guard lock(cache.mutex);
  const auto* entry = cache.Find(number, leaf.image);
  if (!entry || !entry->leaf) {
    ++cache.misses;
    return false;
  }
  ++cache.hits;
  leaf.page = entry->leaf->page;
  leaf.metadata = entry->leaf->metadata;
  return true;
}
// Enters a leaf as read and decoded (never one this write changed).
void CacheLeaf(const CatalogVersionWriterBinding& binding, u64 number, const Leaf& leaf) {
  if (!binding.page_cache) return;
  auto& cache = binding.page_cache->impl();
  std::lock_guard lock(cache.mutex);
  auto& entry = cache.Put(number, leaf.image);
  entry.leaf.emplace();
  entry.leaf->page = leaf.page;
  entry.leaf->metadata = leaf.metadata;
}
// The same for a catalog B-tree page.
bool CachedTreePage(const CatalogVersionWriterBinding& binding, u64 number, const std::vector<byte>& image,
                    pg::NativeBtreePage& page) {
  if (!binding.page_cache) return false;
  auto& cache = binding.page_cache->impl();
  std::lock_guard lock(cache.mutex);
  const auto* entry = cache.Find(number, image);
  if (!entry || !entry->tree) {
    ++cache.misses;
    return false;
  }
  ++cache.hits;
  page = *entry->tree;
  return true;
}
void CacheTreePage(const CatalogVersionWriterBinding& binding, u64 number, const std::vector<byte>& image,
                   const pg::NativeBtreePage& page) {
  if (!binding.page_cache) return;
  auto& cache = binding.page_cache->impl();
  std::lock_guard lock(cache.mutex);
  cache.Put(number, image).tree = page;
}

// SEARCH_KEY: SB_CATALOG_FREE_SPACE (owner Q190, 2026-11-14). Whether `row`
// fits on `page` by the encoder's own layout arithmetic
// (SB_ROW_DATA_PAGE_FOOTPRINT), without encoding the page: the body bytes the
// page uses now plus the row's footprint against the body capacity. A page
// the writer holds carries its body bytes in body.free_space_offset: a page
// decoded from its image (the leaf decoder admits only the image its encoder
// produces, so the figure is the encoder's), or one this write encoded (set
// from the encoder's result); 0 is unknown (a new base leaf). False: the
// encoder would refuse the page as too large, so it is not tried. True or
// nullopt (unknown, or a cell the arithmetic does not size): encode the page;
// the encoder's verdict stands, with every refusal it gives.
// (Was: every candidate page copied, the row added, and the whole page
// encoded to learn whether it fits.)
std::optional<bool> RowFits(const NativeCatalogLeafPage& page, const pg::RowDataRecord& row) {
  if (!page.body.free_space_offset || page.header.page_size_bytes <= 32) return std::nullopt;
  const auto adds = pg::RowDataRecordFootprint(row);
  if (!adds) return std::nullopt;
  return u64{page.body.free_space_offset} + *adds <= pg::RowDataPageBodyCapacity(page.header.page_size_bytes - 32);
}
// Reads the B-tree rooted at `head` through the sink, walked and checked as
// the tree reader walks it; the base leaves its cells name go to `bases`
// (distinct, unsorted). Shared by a relation's index and the name trees
// (SB_NATIVE_NAME_TREES), whose cells name the names relation's leaves.
bool ReadTree(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
              const pg::NativeCatalogRootReference& head, std::vector<TreePage>& tree,
              std::vector<disk::NativePageReference>& bases, CatalogVersionWriteResult& result) {
  const auto& croot = binding.catalog_root;
  const auto refuse = [&](E error) {
    result.error = error;
    return false;
  };
  std::vector<byte> image;
  // The index, walked as the tree reader walks it, read through the sink.
  struct Pending {
    disk::NativePageReference ref;
    std::optional<std::size_t> parent;
    u16 level = 0;
    std::optional<pg::NativeBtreeKey> low, high;
    bool unlisted = false;  // SB_CATALOG_SPLIT_LAG: reached by a sibling link only
  };
  std::vector<Pending> pending{{head.page, {}, 0, {}, {}}};
  std::set<u64> seen;
  std::map<u16, std::size_t> last_by_level;
  std::optional<pg::NativeBtreeDependencies> dependencies;
  while (!pending.empty()) {
    auto next = std::move(pending.back());
    pending.pop_back();
    if (tree.size() >= binding.maximum_index_pages) return refuse(E::resource_exhausted);
    if (next.ref.filespace_uuid != croot.filespace_uuid ||
        next.ref.page_size_profile_uuid != croot.page_size_profile_uuid || !seen.insert(next.ref.page_number).second)
      return refuse(E::invalid_catalog);
    if (!ReadPage(sink, page_size, next.ref.page_number, image, result)) return false;
    const u32 type = !next.parent ? 0x200 : next.level ? 0x201 : 0x202;
    if (!HeaderBinds(image, binding.database_uuid, next.ref, type)) return refuse(E::invalid_catalog);
    // SB_CATALOG_WRITER_PAGE_CACHE: an identical image's decode, or a new one.
    pg::NativeBtreePage page;
    if (!CachedTreePage(binding, next.ref.page_number, image, page)) {
      auto decoded = pg::DecodeNativeBtreePage(image);
      if (!decoded.ok()) return refuse(E::invalid_catalog);
      page = std::move(*decoded.page);
      CacheTreePage(binding, next.ref.page_number, image, page);
    }
    if (!dependencies) {
      if (page.dependencies.index_uuid != head.object_uuid) return refuse(E::invalid_catalog);
      dependencies = page.dependencies;
    } else if (page.dependencies != *dependencies) {
      return refuse(E::invalid_catalog);
    }
    const auto previous = last_by_level.find(page.tree_level);
    // SB_CATALOG_SPLIT_LAG (owner Q45 = 1): as the tree readers walk it.
    const bool lags = next.parent && pg::detail::NativeBtreeSplitLags(page, tree[*next.parent].page, next.high);
    if (pg::detail::ValidateNativeBtreeTraversalStep(
            page, next.parent ? &tree[*next.parent].page : nullptr, next.level, next.low, lags ? page.high_fence : next.high,
            previous == last_by_level.end() ? nullptr : &tree[previous->second].page,
            previous != last_by_level.end() && tree[previous->second].unlisted) != pg::NativeBtreeError::none)
      return refuse(E::invalid_catalog);
    const auto index = tree.size();
    last_by_level[page.tree_level] = index;
    if (lags) pending.push_back({*page.right, next.parent, next.level, page.high_fence, next.high, true});
    tree.push_back({next.ref, image, std::move(page)});
    tree.back().unlisted = next.unlisted;
    tree.back().parent_index = next.parent;
    const auto& current = tree.back().page;
    if (!current.tree_level) {
      for (const auto& cell : current.cells) {
        if (!cell.base_page) return refuse(E::invalid_catalog);
        const auto& ref = *cell.base_page;
        const auto known = std::find_if(bases.begin(), bases.end(),
                                        [&](const auto& b) { return b.page_number == ref.page_number; });
        if (known == bases.end()) bases.push_back(ref);
        else if (*known != ref) return refuse(E::invalid_catalog);
      }
      continue;
    }
    if (!current.first_child || current.cells.empty()) return refuse(E::invalid_catalog);
    const auto child_level = static_cast<u16>(current.tree_level - 1);
    for (std::size_t i = current.cells.size(); i > 0; --i) {
      const auto& cell = current.cells[i - 1];
      if (!cell.child) return refuse(E::invalid_catalog);
      pending.push_back({*cell.child, index, child_level, cell.key,
                         i == current.cells.size() ? current.high_fence : std::optional{current.cells[i].key}});
    }
    pending.push_back({*current.first_child, index, child_level, current.low_fence, current.cells.front().key});
  }
  for (const auto& [level, index] : last_by_level)
    if (tree[index].page.right) return refuse(E::invalid_catalog);
  return true;
}
// SEARCH_KEY: SB_NATIVE_NAME_PROBE (N3). The rows of the leaf cells of a tree
// read by ReadTree (the root first) whose keys begin with `prefix`: one descent
// from the root to the first leaf that may hold such a key, then right along
// the leaf level (sibling links, so a lagging page a parent does not list yet
// is walked too) until a key past the prefix. False when a page the walk
// needs is not among those read (invalid_catalog).
bool ProbeTreePrefix(const std::vector<TreePage>& tree, std::span<const byte> prefix, std::set<Uuid>& rows) {
  if (tree.empty()) return false;
  std::map<u64, std::size_t> at;
  for (std::size_t i = 0; i < tree.size(); ++i) at.emplace(tree[i].ref.page_number, i);
  const auto index_of = [&](const std::optional<disk::NativePageReference>& ref) -> std::optional<std::size_t> {
    if (!ref) return std::nullopt;
    const auto found = at.find(ref->page_number);
    return found == at.end() ? std::nullopt : std::optional{found->second};
  };
  const auto below = [&](std::span<const byte> key) {  // key < prefix
    return std::lexicographical_compare(key.begin(), key.end(), prefix.begin(), prefix.end());
  };
  std::size_t page = 0;
  while (tree[page].page.tree_level) {
    // The last child whose separator is below the prefix; every key with the
    // prefix is above it (no key is the prefix itself), so none is left of it.
    auto child = tree[page].page.first_child;
    for (const auto& cell : tree[page].page.cells) {
      if (!below(cell.key.encoded_key)) break;
      child = cell.child;
    }
    const auto next = index_of(child);
    if (!next) return false;
    page = *next;
  }
  for (;;) {
    for (const auto& cell : tree[page].page.cells) {
      const auto& key = cell.key.encoded_key;
      if (key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin())) {
        rows.insert(cell.key.row_uuid);
      } else if (!below(key)) {
        return true;
      }
    }
    if (!tree[page].page.right) return true;
    const auto next = index_of(tree[page].page.right);
    if (!next) return false;
    page = *next;
  }
}

// Reads the relation `head` names through the sink: the index walked and
// checked as the tree reader walks it, then every base leaf the index names
// (or the head itself, unindexed). False, with `result` set, on a refusal.
bool ReadRelation(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                  const pg::NativeCatalogRootReference& head, Relation& out, CatalogVersionWriteResult& result) {
  const auto& croot = binding.catalog_root;
  const auto refuse = [&](E error) {
    result.error = error;
    return false;
  };
  std::vector<byte> image;
  auto& bases = out.bases;
  if (head.page_type == 6) {
    bases.push_back(head.page);
  } else {
    if (!ReadTree(sink, binding, page_size, head, out.tree, bases, result)) return false;
  }
  if (bases.size() > binding.maximum_base_pages) return refuse(E::resource_exhausted);
  std::sort(bases.begin(), bases.end(),
            [](const auto& a, const auto& b) { return a.page_number < b.page_number; });

  auto& leaves = out.leaves;
  leaves.assign(bases.size(), Leaf{});
  std::optional<Uuid> relation;
  for (std::size_t i = 0; i < bases.size(); ++i) {
    const auto& ref = bases[i];
    if (ref.filespace_uuid != croot.filespace_uuid || ref.page_size_profile_uuid != croot.page_size_profile_uuid)
      return refuse(E::invalid_catalog);
    if (!ReadPage(sink, page_size, ref.page_number, leaves[i].image, result)) return false;
    if (!HeaderBinds(leaves[i].image, binding.database_uuid, ref, 6)) return refuse(E::invalid_catalog);
    // SB_CATALOG_WRITER_PAGE_CACHE: an identical image's decode, or a new one.
    if (!CachedLeaf(binding, ref.page_number, leaves[i])) {
      auto decoded = DecodeNativeCatalogLeaf(leaves[i].image);
      if (!decoded.ok()) {
        result.error = E::invalid_catalog;
        result.leaf_error = decoded.error;
        return false;
      }
      leaves[i].page = std::move(*decoded.page);
      leaves[i].metadata = std::move(decoded.metadata);
      CacheLeaf(binding, ref.page_number, leaves[i]);
    }
    const auto& id = leaves[i].page.body.relation_uuid.value;
    if (head.page_type == 6 ? id != head.object_uuid : relation && *relation != id) return refuse(E::invalid_catalog);
    relation = id;
  }

  return true;
}

// A row's hold on its object: none once its creator rolled back or died;
// pending while another transaction that created it is in progress;
// otherwise held (committed, or this transaction's own).
enum class Hold { none, pending, held };

// ---- names: what a transaction sees of profiles, policies and names ---------
// The insert and successor writers' one view of the names relation (role 2)
// and the profiles and policies an entry names (role 5), from the images read
// for this request: every stored name version with its envelope checked, the
// newest version of each name row, the entries that may hold a name, and the
// names a path may run through (SB_NATIVE_NAME_PATH). SB_CATALOG_SINGLE_WRITER:
// what it decides holds only while no other catalog writer runs between the
// read and the write.
class NameState {
 public:
  using EntryFn = std::function<const mga::TransactionInventoryEntry*(u64)>;
  // Every stored name version, its envelope checked against where it is.
  struct StoredName {
    catalog::CatalogNamePayload payload;
    Uuid row, object;
    u64 row_version = 0, order = 0;
    std::optional<mga::TransactionState> state;
    bool deleted = false;
  };
  // The collision group an entry's class binds to, as startup resolves it:
  // vector -> policy descriptor -> naming collision policy body -> class.
  struct Group {
    Uuid body, group;
    catalog::CatalogCollisionLanguageLane language{};
    catalog::CatalogCollisionKeyLane key{};
  };
  struct Verdict {
    E error = E::none;
    std::optional<u64> blocker;
    // SB_NATIVE_NAME_ROW_V4: on success, the entry's own-lane fold under its
    // identifier profile (the cell key's lane), so the profile is resolved
    // once per written entry.
    std::optional<catalog::IdentifierFoldMode> own_fold;
  };
  // `request_names`: the request's own name payloads (an insert's rows, a
  // successor's one), consulted for vectors and paths before stored names.
  NameState(const CatalogVersionWriterBinding& binding, u64 order, const std::map<u16, Relation>& relations,
            EntryFn entry, std::vector<const catalog::CatalogNamePayload*> request_names)
      : binding_(binding), order_(order), relations_(relations), entry_(std::move(entry)),
        request_names_(std::move(request_names)) {}

  // Reads every stored name version of role 2 when loaded; false when one does
  // not decode against where it is (invalid_catalog).
  bool Load() {
    if (relations_.contains(2)) {
      for (const auto& leaf : relations_.at(2).leaves)
        for (const auto& row : leaf.page.body.rows) {
          const auto m = leaf.metadata.find(row.version_uuid);
          if (m == leaf.metadata.end()) return false;
          const catalog::CatalogNameVersionBinding at{
              {UuidKind::database, leaf.page.header.database_uuid}, {UuidKind::filespace, leaf.page.header.filespace_uuid},
              row.row_uuid, {UuidKind::row, row.version_uuid}, m->second.record.header.object_uuid, row.transaction_uuid,
              leaf.page.body.page_number, row.stable_slot_id, row.storage_generation, row.row_version,
              row.local_transaction_id, m->second.catalog_generation};
          const auto& bytes = m->second.record.payload;
          const auto decoded = catalog::DecodeCatalogNameEnvelopeView(
              {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, at);
          if (!decoded.ok()) return false;
          stored_.push_back({catalog::MaterializeCatalogNameEnvelope(*decoded.record).payload, row.row_uuid.value,
                             m->second.record.header.object_uuid.value, row.row_version, row.local_transaction_id,
                             StateOf(row), m->second.record.header.deleted});
        }
      for (std::size_t k = 0; k < stored_.size(); ++k) {
        if (Dead(stored_[k].state)) continue;
        const auto found = newest_name_.find(stored_[k].row);
        if (found == newest_name_.end() || stored_[found->second].row_version < stored_[k].row_version)
          newest_name_[stored_[k].row] = k;
      }
    }
    // The entries that may hold a name: each stored entry row's newest version
    // not rolled back or dead, then this request's entries as placed.
    for (const auto& [row, k] : newest_name_) {
      const auto& s = stored_[k];
      const auto* e = std::get_if<catalog::CatalogNameEntry>(&s.payload);
      if (!e) continue;
      if (s.state && InProgress(*s.state) && s.order != order_) {
        holders_.push_back({&s.payload, Hold::pending, s.order, row});
        continue;
      }
      if (s.deleted || e->lifecycle_state == catalog::CatalogNameLifecycle::dropped ||
          e->lifecycle_state == catalog::CatalogNameLifecycle::archived)
        continue;
      holders_.push_back({&s.payload, Hold::held, s.order, row});
    }
    // SB_NATIVE_NAME_PATH: the names a path through this request may use: the
    // newest version of each stored name this transaction sees (committed or
    // its own, not deleted), then the request's names. Q47 = 1: a row whose
    // newest version is another transaction's in progress is seen as its
    // newest committed version (it was dropped from the index outright, which
    // left everything below that name unnamed to an unrelated writer).
    // SB_NATIVE_NAME_CONFLICT_WRITE_WRITE_ONLY (owner Q226, 2026-11-15): such a
    // row is no longer recorded for any path check of another transaction.
    // (Was, Q212: also recorded as contended, for PathConflict; before that,
    // Q47, for PendingOnPath.)
    for (std::size_t k = 0; k < stored_.size(); ++k) {
      const auto& s = stored_[k];
      if (!s.state || (*s.state != mga::TransactionState::committed && s.order != order_)) continue;
      const auto found = visible_name_.find(s.row);
      if (found == visible_name_.end() || stored_[found->second].row_version < s.row_version) visible_name_[s.row] = k;
    }
    for (const auto& [row, k] : visible_name_)
      if (!stored_[k].deleted) path_names_.Add(catalog::BorrowCatalogNamePayload(stored_[k].payload));
    for (const auto* name : request_names_) path_names_.Add(catalog::BorrowCatalogNamePayload(*name));
    return true;
  }

  // SEARCH_KEY: SB_NATIVE_NAME_CONFLICT_WRITE_WRITE_ONLY (owner Q226,
  // 2026-11-15: "a rename does not effect its actual uuid - in SBLR it is
  // referred to by UUID ... A rename only really causes an issue if it is a
  // write-write situation on the name. Transaction isolation will not see the
  // new name until the current transaction commits or rolls-back"). A name
  // write conflicts with another transaction in progress only (1) write-write
  // on the same name row (the successor's base, an insert's row or object) and
  // (2) name-key uniqueness in a scope (CheckEntry's holders: another
  // transaction's in-progress entry with the same own-lane key in the same
  // scope, identifier profile, language lane and collision group; the second
  // writer gets write_conflict, Firebird unique-key style). Paths are checked
  // only in this transaction's own view (CheckEntry, CheckPathsAfterChange).
  // Drop-versus-create races under a scope are for dependency rows under MGA
  // write-write control (owner Q226), not for names.
  // (Was, SB_NATIVE_NAME_PATH_CONFLICT_BREAK, owner Q212, 2026-11-15:
  // PathConflict - an entry E and another transaction C changing name rows,
  // both in progress, conflicted when E's path walked in one ending of C and
  // not the other, so a drop or a second default that broke E's path was a
  // write_conflict; with Decides and FirstOnPath, the lookups a row decided.
  // Before that, Q47 = 1, PendingOnPath: a write_conflict with the first
  // transaction whose old or new version decided any lookup on the path.)

  // The newest version of an object this transaction sees (its creator
  // committed, or its own), unless deleted.
  const catalog::CatalogMetadataVersion* Current(u16 role, const Uuid& object) const {
    const auto rel = relations_.find(role);
    if (rel == relations_.end()) return nullptr;
    const catalog::CatalogMetadataVersion* best = nullptr;
    u64 newest = 0;
    for (const auto& leaf : rel->second.leaves)
      for (const auto& row : leaf.page.body.rows) {
        const auto m = leaf.metadata.find(row.version_uuid);
        if (m == leaf.metadata.end() || m->second.record.header.object_uuid.value != object) continue;
        const auto state = StateOf(row);
        if (!state || (*state != mga::TransactionState::committed && row.local_transaction_id != order_)) continue;
        if (!best || row.row_version > newest) {
          best = &m->second;
          newest = row.row_version;
        }
      }
    return best && !best->record.header.deleted ? best : nullptr;
  }
  // The vector an entry names: in the request, else the newest stored
  // version (whoever wrote it: another transaction's entry in progress is
  // judged by its own vector).
  const catalog::CatalogNameVector* VectorOf(const Uuid& id) const {
    for (const auto* name : request_names_)
      if (const auto* v = std::get_if<catalog::CatalogNameVector>(name); v && v->name_vector_uuid.value == id) return v;
    for (const auto& [row, k] : newest_name_)
      if (stored_[k].object == id && !stored_[k].deleted)
        if (const auto* v = std::get_if<catalog::CatalogNameVector>(&stored_[k].payload)) return v;
    return nullptr;
  }
  // SB_CATALOG_WRITER_NAME_GROUP_MEMO (streaming work, 2026-11-25): the group an entry's class binds to depends only on
  // its name vector, object and kind, and on rows fixed for this request (the request's vectors, the stored newest
  // versions, the configuration relation), so it is decided once per distinct (vector, object, kind) and remembered
  // for this NameState's life. Before this every holder of a collision probe re-ran VectorOf (a scan of every name
  // row) and three scans of the configuration leaves per holder: about 28 ms per inserted name at 1100 rows, growing
  // with the scope, which made a names relation of thousands of rows unwritable in practice.
  std::optional<Group> GroupOf(const catalog::CatalogNameEntry& e) const {
    const auto key = std::make_tuple(e.name_vector_uuid.value, e.object_uuid.value, static_cast<u32>(e.object_kind));
    if (const auto hit = group_memo_.find(key); hit != group_memo_.end()) return hit->second;
    auto computed = ComputeGroupOf(e);
    group_memo_.emplace(key, computed);
    return computed;
  }
  std::optional<Group> ComputeGroupOf(const catalog::CatalogNameEntry& e) const {
    const auto* v = VectorOf(e.name_vector_uuid.value);
    if (!v || v->object_uuid.value != e.object_uuid.value || v->object_kind != e.object_kind) return std::nullopt;
    const auto* dm = Current(5, v->name_collision_policy_uuid.value);
    if (!dm || !Active(*dm) || !catalog::CatalogPolicyDescriptorMatchesMetadata(View(*dm)) ||
        !catalog::CatalogPolicyDescriptorMatchesDatabase(View(*dm), binding_.database_uuid))
      return std::nullopt;
    const auto policy = catalog::DecodeCatalogPolicyDescriptor(dm->record.payload);
    if (!policy.record || policy.record->policy_class != catalog::CatalogPolicyClass::metadata ||
        policy.record->scope != catalog::CatalogPolicyScope::database ||
        policy.record->scope_uuid != binding_.database_uuid)
      return std::nullopt;
    const auto* bm = Current(5, policy.record->body_uuid);
    if (!bm || !Active(*bm) ||
        !(bm->object_subtype == scratchbird::core::catalog::CatalogObjectSubtype::naming_collision_policy_body ||
          catalog::IsCatalogNamingCollisionPolicyPayload(bm->record.payload)) ||
        !catalog::CatalogNamingCollisionPolicyMatchesMetadata(View(*bm)) ||
        !catalog::CatalogNamingCollisionPolicyMatchesDatabase(View(*bm), binding_.database_uuid))
      return std::nullopt;
    const auto body = catalog::DecodeCatalogNamingCollisionPolicy(bm->record.payload);
    const auto kind = KnownKind(e.object_kind);
    if (!body.ok() || body.record->generation != policy.record->body_generation || !kind) return std::nullopt;
    const auto classes = std::span(body.record->classes).first(body.record->class_count);
    const auto bound = std::find_if(classes.begin(), classes.end(), [&](const auto& c) { return c.kind == *kind; });
    if (bound == classes.end()) return std::nullopt;
    const auto groups = std::span(body.record->groups).first(body.record->group_count);
    const auto group = std::find_if(groups.begin(), groups.end(),
                                    [&](const auto& g) { return g.group_uuid == bound->group_uuid; });
    if (group == groups.end()) return std::nullopt;
    return Group{policy.record->body_uuid, group->group_uuid, group->language_lane, group->key_lane};
  }
  // An entry's identifier profile, as startup checks it: its dialect profile,
  // that profile's identifier profile (active, this database's, the dialect's
  // generation and mode flags). Nullopt when any of that does not hold.
  std::optional<catalog::CatalogIdentifierResolutionProfile> IdentifierProfileOf(const catalog::CatalogNameEntry& e) const {
    if (e.case_fold_profile_uuid || e.quoted_identifier_profile_uuid) return std::nullopt;
    const auto* dm = Current(5, e.dialect_profile_uuid.value);
    if (!dm || !Active(*dm) || dm->record.header.kind != catalog::CatalogRecordKind::config_profile ||
        dm->object_subtype != scratchbird::core::catalog::CatalogObjectSubtype::naming_dialect_profile || !catalog::CatalogNamingDialectProfileMatchesMetadata(View(*dm)) ||
        !catalog::CatalogNamingDialectProfileMatchesDatabase(View(*dm), binding_.database_uuid))
      return std::nullopt;
    const auto dialect = catalog::DecodeCatalogNamingDialectProfile(dm->record.payload);
    if (!dialect.record || dialect.record->identifier_profile_uuid != e.identifier_profile_uuid.value) return std::nullopt;
    const auto* im = Current(5, dialect.record->identifier_profile_uuid);
    if (!im || !Active(*im) || im->record.header.kind != catalog::CatalogRecordKind::config_profile ||
        im->object_subtype != scratchbird::core::catalog::CatalogObjectSubtype::identifier_resolution_profile ||
        !catalog::CatalogIdentifierProfileMatchesMetadata(View(*im)) ||
        !catalog::CatalogIdentifierProfileMatchesDatabase(View(*im), binding_.database_uuid))
      return std::nullopt;
    auto identifier = catalog::DecodeCatalogIdentifierProfile(im->record.payload);
    if (!identifier.record || identifier.record->generation != dialect.record->identifier_profile_generation ||
        (identifier.record->dialect_mode_flags & ~dialect.record->allowed_dialect_mode_flags))
      return std::nullopt;
    return std::move(*identifier.record);
  }
  // An entry about to be written: its keys (derived under its profile,
  // SB_NATIVE_NAME_ROW_V4), that its path walks (was: its stored full path), then the
  // names it could collide with (another object's held entry in its scope and
  // lanes is duplicate_name; one in progress is a write_conflict with that
  // blocker; a holder whose group does not resolve is invalid_catalog unless it
  // is still pending).
  Verdict CheckEntry(const catalog::CatalogNamePayload& payload) const {
    const auto& x = std::get<catalog::CatalogNameEntry>(payload);
    // SB_PART_B_S6_NAME_ADMISSION: the engine's one text rule first, so each refusal has its own code (the key
    // derivation below applies the same rule and would only say invalid_name). "" passes: it is a name.
    if (const auto text = NameTextRefusal(payload)) return {*text};  // one helper (also applied before the record codec, below)
    const auto gx = GroupOf(x);
    if (!gx) return {E::invalid_name};
    const auto profile = IdentifierProfileOf(x);
    if (!profile) return {E::invalid_name};
    const auto xv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(payload));
    const auto xkeys = catalog::DeriveCatalogNameEntryKeys(*profile, xv);
    if (!xkeys.ok()) return {E::invalid_name};
    // Owner Q226 (SB_NATIVE_NAME_CONFLICT_WRITE_WRITE_ONLY): the path walks in
    // this transaction's view only. (Was, Q212: first PathConflict, a
    // write_conflict with another transaction whose ending decided whether it
    // walked; Q47: PendingOnPath.)
    if (catalog::CheckCatalogNameEntryPath(xv, path_names_) != catalog::CatalogNamePathError::none) return {E::invalid_name};
    // SB_NATIVE_NAME_PROBE (N3): with the authoritative tree, the candidates
    // are the rows its (scope, identifier profile) range holds - every entry
    // version in the scope has a cell there, so each row whose holding version
    // is in the scope is found (through any of its versions' cells); each is
    // then judged by its holding version, as before. That range, not the
    // name's key: startup judges every entry of the scope (a class bound under
    // two policy bodies), and the ASCII-folded and all-languages lanes compare
    // across language tags. The request's own entries are not in the tree yet.
    //
    // SB_CATALOG_WRITER_SCOPE_INDEX (streaming work, 2026-11-28): the scope's holders are judged ONCE for this
    // NameState (ScopeFor: the tree range probed once, each holder's group and lanes resolved once) and indexed; an
    // entry is then checked against the holders the loop below would have stopped at, found by key. The verdicts are
    // those of the scan this replaces (the scan, in holder order, for each holder of another object: no group ->
    // invalid_catalog, or a blocker when pending; the entry's kind under another policy body -> invalid_name; a
    // different group -> passed over; lanes the profile does not admit -> invalid_catalog, or a blocker when pending;
    // lanes equal -> held, or a blocker when pending; a refusal at any holder beats held and blockers, the first
    // blocker in holder order is reported). Before this every inserted name walked every holder of its scope:
    // O(scope) per name.
    const ScopeIndex* ix = ScopeFor(xv, *profile, xkeys.own);
    if (!ix) return {E::invalid_catalog};
    constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
    std::size_t fatal_at = kNone, blocker_at = kNone;
    E fatal_error = E::none;
    const auto refuse_at = [&](std::size_t member, E error) {
      if (ix->members[member].at < fatal_at) {
        fatal_at = ix->members[member].at;
        fatal_error = error;
      }
    };
    const auto pend_at = [&](std::size_t member) { blocker_at = std::min(blocker_at, ix->members[member].at); };
    // The holder at `member` stops the check (invalid_catalog) unless it is pending, which only makes a blocker.
    const auto unresolvable = [&](std::size_t member) {
      if (ix->members[member].object == x.object_uuid.value) return;
      if (holders_[ix->members[member].at].state != Hold::pending) refuse_at(member, E::invalid_catalog);
      else pend_at(member);
    };
    for (const auto member : ix->unresolved) unresolvable(member);
    if (const auto bad = ix->bad_keys.find({gx->body, gx->group}); bad != ix->bad_keys.end())
      for (const auto member : bad->second) unresolvable(member);
    if (const auto kind = ix->bodies.find(static_cast<u32>(x.object_kind)); kind != ix->bodies.end())
      for (const auto& [body, members] : kind->second) {
        if (body == gx->body) continue;
        for (const auto member : members)
          if (ix->members[member].object != x.object_uuid.value) {  // the entry's class bound under another policy body
            refuse_at(member, E::invalid_name);
            break;
          }
      }
    bool held = false;
    if (const auto same = ix->by_text.find(TextKey(gx->body, gx->group, xkeys.own)); same != ix->by_text.end())
      for (const auto member : same->second) {
        const auto& m = ix->members[member];
        if (m.object == x.object_uuid.value) continue;  // an alias of the same object
        const auto& holder = holders_[m.at];
        const auto yv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(*holder.payload));
        const auto& ykeys = HolderKeys(*ix->profile, x.identifier_profile_uuid.value, holder.payload, yv);
        if (catalog::CompareCatalogNameCollisionLanes(xv, xkeys.own, yv, ykeys.own, gx->language, gx->key)) continue;
        if (holder.state == Hold::held) held = true;
        else pend_at(member);
      }
    if (fatal_at != kNone) return {fatal_error};
    if (held) return {E::duplicate_name};
    if (blocker_at != kNone) return {E::write_conflict, holders_[blocker_at].order};
    return {E::none, std::nullopt, xkeys.own.fold};
  }
  // SB_CATALOG_WRITER_NAME_GROUP_MEMO (streaming work, 2026-11-25): a holder's lanes under an identifier profile depend only
  // on the holder's own entry, so they are derived once per (holder, profile) for this NameState, not once per entry
  // written against it (an insert of N names probed every holder of the scope N times: O(N x scope) derivations).
  const catalog::CatalogNameEntryKeys& HolderKeys(const catalog::CatalogIdentifierResolutionProfile& profile, const Uuid& profile_id,
                                                  const catalog::CatalogNamePayload* payload,
                                                  const catalog::CatalogNameEntryView& view) const {
    const auto key = std::make_pair(static_cast<const void*>(payload), profile_id);
    auto it = holder_keys_.find(key);
    if (it == holder_keys_.end()) it = holder_keys_.emplace(key, catalog::DeriveCatalogNameEntryKeys(profile, view)).first;
    return it->second;
  }
  // This request's entry, once placed: later entries of the request see it.
  void HoldEntry(const catalog::CatalogNamePayload& payload) {
    holders_.push_back({&payload, Hold::held, order_, std::nullopt});
    // SB_CATALOG_WRITER_SCOPE_INDEX: a scope already indexed takes the entry as its newest holder.
    if (const auto* e = std::get_if<catalog::CatalogNameEntry>(&payload))
      if (const auto it = scopes_.find({e->scope_uuid.value, e->identifier_profile_uuid.value}); it != scopes_.end())
        AddMember(it->second, holders_.size() - 1);
  }
  // SB_NATIVE_NAME_PROBE (N3): the authoritative name tree (kind 22) as read
  // for this request; CheckEntry takes its candidates from it. Without one
  // (a database without name trees: component fixtures, owner Q44 = 1) every
  // holder is a candidate.
  void ProbeWith(const std::vector<TreePage>* authoritative) {
    authoritative_ = authoritative;
    scopes_.clear();
  }
  // SB_NATIVE_NAME_DEFAULT_INDEX (owner Q228 revisited, Q259): the UUID ->
  // name tree (kind 23) as read for this request; CheckDefault takes its
  // candidates from the object's entry range there. Without one every stored
  // entry is a candidate.
  void DefaultIndexWith(const std::vector<TreePage>* uuid_tree) { uuid_tree_ = uuid_tree; }
  static bool LiveName(const StoredName& s) {
    const auto state = std::visit([](const auto& r) { return r.lifecycle_state; }, s.payload);
    return !s.deleted && state != catalog::CatalogNameLifecycle::dropped && state != catalog::CatalogNameLifecycle::archived;
  }
  // SEARCH_KEY: SB_NATIVE_NAME_DEFAULT_INDEX. Whether `x` (an entry about to
  // be written; `self_row` its row for a successor) may hold the defaults it
  // is marked with: at most one live default_for_language entry per (object,
  // language) and one default_for_object entry per object, the object
  // default also being its language's default (else invalid_name).
  // Unique-index semantics over every version not rolled back or dead
  // (FirebirdSQL keeps an index entry per record version): a candidate row
  // whose version this transaction sees (committed, or its own newest) holds
  // the default: duplicate_default_name; a row whose newest version is another
  // transaction's in progress conflicts when that version or the committed
  // one below it holds the default (write_conflict with it): if it rolls back
  // the committed default stands, if it commits its own does. The request's
  // own earlier entries (HoldEntry) are held.
  // `other`: the paired row of a default switch (SB_NATIVE_NAME_DEFAULT_SWITCH)
  // and its new version, which stands for that row.
  Verdict CheckDefault(const catalog::CatalogNameEntry& x, const std::optional<Uuid>& self_row,
                       const std::pair<Uuid, const catalog::CatalogNameEntry*>* other = nullptr) const {
    if (!x.default_for_language && !x.default_for_object) return {};
    if (x.default_for_object && !x.default_for_language) return {E::invalid_name};
    const auto conflicts = [&](const catalog::CatalogNameEntry& y) {
      if (y.object_uuid.value != x.object_uuid.value || y.name_entry_uuid.value == x.name_entry_uuid.value) return false;
      return (x.default_for_language && y.default_for_language && y.language_uuid.value == x.language_uuid.value) ||
             (x.default_for_object && y.default_for_object);
    };
    std::set<Uuid> probed;
    const bool probe = uuid_tree_ != nullptr;
    if (probe) {
      const auto kind = KnownKind(x.object_kind);
      if (!kind) return {E::invalid_name};
      // object | kind | 0x01: every entry of the object (the per-language
      // range, object | kind | 0x01 | language | {0, 1}, does not cover the
      // object default, which is unique across languages).
      auto prefix = catalog::CatalogNameUuidTreeLanguagePrefix(x.object_uuid.value, *kind, x.language_uuid.value);
      prefix.resize(19);
      if (!ProbeTreePrefix(*uuid_tree_, prefix, probed)) return {E::invalid_catalog};
    }
    bool held = false;
    std::optional<u64> blocker;
    for (const auto& [row, k] : newest_name_) {
      if ((self_row && row == *self_row) || (other && row == other->first) || (probe && !probed.contains(row))) continue;
      const auto& s = stored_[k];
      const auto* y = std::get_if<catalog::CatalogNameEntry>(&s.payload);
      if (!y) continue;
      if (s.state && InProgress(*s.state) && s.order != order_) {
        bool c = LiveName(s) && conflicts(*y);
        if (const auto seen = visible_name_.find(row); seen != visible_name_.end()) {
          const auto& v = stored_[seen->second];
          if (const auto* w = std::get_if<catalog::CatalogNameEntry>(&v.payload); w && LiveName(v) && conflicts(*w)) c = true;
        }
        if (c && !blocker) blocker = s.order;
        continue;
      }
      if (LiveName(s) && conflicts(*y)) held = true;  // committed, or this transaction's own newest
    }
    for (const auto& holder : holders_)
      if (!holder.row)
        if (const auto* y = std::get_if<catalog::CatalogNameEntry>(holder.payload); y && conflicts(*y)) held = true;
    if (other && other->second && conflicts(*other->second)) held = true;
    if (held) return {E::duplicate_default_name};
    if (blocker) return {E::write_conflict, blocker};
    return {};
  }
  const catalog::CatalogNameIndex& Paths() const { return path_names_; }
  // SB_NATIVE_NAME_CASCADE (N4b): the names a path may run through as stored
  // (the newest version of each name row this transaction sees, not deleted:
  // the rows Paths() holds), with `row`'s replaced by `replacement` (none:
  // removed). The payloads must outlive the index.
  // Q48 = 1: an insert's `added` names are added after the stored ones.
  // (Was, Q212, SB_NATIVE_NAME_PATH_CONFLICT_BREAK: a `with` transaction
  // order, which also took that transaction's name rows in progress, for
  // PathConflict; removed by owner Q226, SB_NATIVE_NAME_CONFLICT_WRITE_WRITE_ONLY.)
  catalog::CatalogNameIndex StoredPaths(const std::optional<Uuid>& row, const catalog::CatalogNamePayload* replacement,
                                        std::span<const catalog::CatalogNamePayload* const> added = {}) const {
    catalog::CatalogNameIndex out;
    for (const auto& [r, k] : visible_name_) {
      if (row && r == *row) continue;
      if (!stored_[k].deleted) out.Add(catalog::BorrowCatalogNamePayload(stored_[k].payload));
    }
    if (replacement) out.Add(catalog::BorrowCatalogNamePayload(*replacement));
    for (const auto* name : added) out.Add(catalog::BorrowCatalogNamePayload(*name));
    return out;
  }
  // Per name row, the version this transaction sees (committed, or its own),
  // as an index into Stored(); deleted versions included.
  const std::map<Uuid, std::size_t>& Visible() const { return visible_name_; }
  std::span<const StoredName> Stored() const { return stored_; }

 private:
  struct Holder {
    const catalog::CatalogNamePayload* payload;
    Hold state;
    u64 order;
    std::optional<Uuid> row;  // a stored entry's row; none for the request's own
  };
  // SB_CATALOG_WRITER_SCOPE_INDEX: one (scope, identifier profile)'s holders, each judged once.
  struct ScopeMember {
    std::size_t at = 0;  // its place in holders_
    Uuid object;
    u32 kind = 0;
    std::optional<Group> group;  // none: the group does not resolve
    bool keys_ok = false;        // its lanes derive under the scope's profile
  };
  struct ScopeIndex {
    std::optional<catalog::CatalogIdentifierResolutionProfile> profile;
    std::vector<ScopeMember> members;                                    // in holder order
    std::vector<std::size_t> unresolved;                                 // members with no group
    std::map<u32, std::map<Uuid, std::vector<std::size_t>>> bodies;      // object kind -> policy body -> members with a group
    std::map<std::pair<Uuid, Uuid>, std::vector<std::size_t>> bad_keys;  // (body, group) -> members whose lanes do not derive
    std::unordered_map<std::string, std::vector<std::size_t>> by_text;   // (body, group, own-lane text) -> members
  };
  static std::string TextKey(const Uuid& body, const Uuid& group, const catalog::CatalogNameDerivedText& own) {
    std::string key;
    key.reserve(32 + own.size());
    key.append(reinterpret_cast<const char*>(body.bytes.data()), body.bytes.size());
    key.append(reinterpret_cast<const char*>(group.bytes.data()), group.bytes.size());
    for (std::size_t i = 0; i < own.size(); ++i) key.push_back(static_cast<char>(own[i]));
    return key;
  }
  void AddMember(ScopeIndex& ix, std::size_t at) const {
    const auto& holder = holders_[at];
    const auto& y = std::get<catalog::CatalogNameEntry>(*holder.payload);
    ScopeMember m;
    m.at = at;
    m.object = y.object_uuid.value;
    m.kind = static_cast<u32>(y.object_kind);
    m.group = GroupOf(y);
    const auto position = ix.members.size();
    if (!m.group) {
      ix.members.push_back(m);
      ix.unresolved.push_back(position);
      return;
    }
    ix.bodies[m.kind][m.group->body].push_back(position);
    const auto yv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(*holder.payload));
    const auto& ykeys = HolderKeys(*ix.profile, y.identifier_profile_uuid.value, holder.payload, yv);
    m.keys_ok = ykeys.ok();
    const auto body = m.group->body, group = m.group->group;
    ix.members.push_back(m);
    if (m.keys_ok) ix.by_text[TextKey(body, group, ykeys.own)].push_back(position);
    else ix.bad_keys[{body, group}].push_back(position);
  }
  // The index of `x`'s scope, built on first use: with the authoritative tree, the holders whose row its (scope,
  // identifier profile) range holds (and the request's own), each then held to the scope order. Null when the
  // tree walk fails (invalid_catalog).
  const ScopeIndex* ScopeFor(const catalog::CatalogNameEntryView& xv, const catalog::CatalogIdentifierResolutionProfile& profile,
                             const catalog::CatalogNameDerivedText& xown) const {
    const std::pair<Uuid, Uuid> key{xv.scope_uuid.value, xv.identifier_profile_uuid.value};
    if (const auto it = scopes_.find(key); it != scopes_.end()) return &it->second;
    std::set<Uuid> probed;
    if (authoritative_) {
      auto prefix = catalog::CatalogNameAuthoritativeTreeNamePrefix(xv, xown);
      prefix.resize(32);  // scope16 | identifier profile16
      if (!ProbeTreePrefix(*authoritative_, prefix, probed)) return nullptr;
    }
    ScopeIndex ix;
    ix.profile = profile;
    for (std::size_t at = 0; at < holders_.size(); ++at) {
      const auto& holder = holders_[at];
      if (authoritative_ && holder.row && !probed.contains(*holder.row)) continue;  // outside the scope
      const auto yv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(*holder.payload));
      if (catalog::CompareCatalogNameCollisionScope(xv, yv)) continue;
      AddMember(ix, at);
    }
    return &scopes_.emplace(key, std::move(ix)).first->second;
  }
  mutable std::map<std::pair<Uuid, Uuid>, ScopeIndex> scopes_;  // SB_CATALOG_WRITER_SCOPE_INDEX
  const std::vector<TreePage>* authoritative_ = nullptr;
  const std::vector<TreePage>* uuid_tree_ = nullptr;  // SB_NATIVE_NAME_DEFAULT_INDEX
  std::optional<mga::TransactionState> StateOf(const pg::RowDataRecord& row) const {
    const auto* creator = entry_(row.local_transaction_id);
    if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value) return std::nullopt;
    return mga::InventoryVisibilityState(*creator);
  }
  static bool Dead(const std::optional<mga::TransactionState>& state) {
    return state && (*state == mga::TransactionState::rolled_back || *state == mga::TransactionState::failed_terminal);
  }
  static bool Active(const catalog::CatalogMetadataVersion& m) {
    return m.status == catalog::CatalogObjectStatus::active && m.lifecycle == catalog::CatalogObjectLifecycle::active;
  }
  static catalog::CatalogMetadataVersionView View(const catalog::CatalogMetadataVersion& m) {
    return catalog::BorrowCatalogMetadataVersion(m);
  }
  const CatalogVersionWriterBinding& binding_;
  u64 order_;
  const std::map<u16, Relation>& relations_;
  EntryFn entry_;
  std::vector<const catalog::CatalogNamePayload*> request_names_;
  std::vector<StoredName> stored_;
  std::map<Uuid, std::size_t> newest_name_;  // per row: the newest version not rolled back or dead
  std::map<Uuid, std::size_t> visible_name_;  // per row: the newest version committed or this transaction's
  // (Was, Q47/Q212: contended_, each name row whose newest version was
  // another transaction's in progress, for PathConflict; owner Q226.)
  std::vector<Holder> holders_;
  mutable std::map<std::tuple<Uuid, Uuid, u32>, std::optional<Group>> group_memo_;  // SB_CATALOG_WRITER_NAME_GROUP_MEMO
  mutable std::map<std::pair<const void*, Uuid>, catalog::CatalogNameEntryKeys> holder_keys_;  // SB_CATALOG_WRITER_NAME_GROUP_MEMO
  catalog::CatalogNameIndex path_names_;
};

// SEARCH_KEY: SB_CATALOG_NEW_PAGES (W3b-3a, owner Q30 = 1). The pages one
// write adds, taken in order. A write runs twice when it needs pages: a dry
// pass, which decides everything and refuses before any write, hands out
// placeholder numbers and records what each page will be; the pages are then
// allocated, each durable on its own; and the real pass, given the allocated
// numbers, builds the images and runs the write's plan. A write that needs no
// page runs once.
class PageSource {
 public:
  struct Need {
    u32 page_type = 0;
    Uuid owner;
  };
  // Dry: placeholders past the file's end; a real pass: these numbers.
  PageSource(std::span<const CatalogNewPageIdentity> identities, u64 placeholder_base)
      : identities_(identities), next_placeholder_(placeholder_base) {}
  PageSource(std::span<const CatalogNewPageIdentity> identities, std::vector<u64> numbers, std::vector<Need> needs)
      : identities_(identities), numbers_(std::move(numbers)), expected_(std::move(needs)), dry_(false) {}
  struct Taken {
    u64 page_number = 0;
    Uuid page_uuid;
  };
  // Nullopt when the request has no identity left (new_pages_exhausted), or
  // when a real pass asks for a page the dry pass did not (the passes
  // disagree: the caller refuses).
  std::optional<Taken> Take(u32 page_type, const Uuid& owner) {
    const auto at = used_;
    if (at >= identities_.size()) return std::nullopt;
    if (dry_) {
      needs_.push_back({page_type, owner});
      ++used_;
      return Taken{next_placeholder_++, identities_[at].page_uuid};
    }
    if (at >= numbers_.size() || expected_[at].page_type != page_type || expected_[at].owner != owner) {
      disagreed_ = true;
      return std::nullopt;
    }
    ++used_;
    return Taken{numbers_[at], identities_[at].page_uuid};
  }
  bool exhausted_by(std::size_t index) const { return index >= identities_.size(); }
  bool dry() const { return dry_; }
  bool needed() const { return dry_ && !needs_.empty(); }
  const std::vector<Need>& needs() const { return needs_; }
  std::size_t used() const { return used_; }
  bool disagreed() const { return disagreed_; }

 private:
  std::span<const CatalogNewPageIdentity> identities_;
  std::vector<u64> numbers_;
  std::vector<Need> expected_, needs_;
  u64 next_placeholder_ = 0;
  std::size_t used_ = 0;
  bool dry_ = true, disagreed_ = false;
};

// A new, empty base leaf of the relation `like` belongs to, at `taken`:
// `like`'s header and body identity with this page's number, UUID and
// generation 1, and no rows. Its "as read" image is what the file holds there
// now (the real pass reads it; the dry pass never writes).
std::optional<Leaf> NewBaseLeaf(disk::CarefulWriteSink& sink, const NativeCatalogLeafPage& like, u32 page_size,
                                const PageSource::Taken& taken, bool read_image, CatalogVersionWriteResult& result) {
  Leaf leaf;
  leaf.page.header = like.header;
  leaf.page.header.page_uuid = taken.page_uuid;
  leaf.page.header.page_number = taken.page_number;
  leaf.page.header.page_generation = 1;
  leaf.page.body = like.body;
  leaf.page.body.rows.clear();
  leaf.page.body.page_number = taken.page_number;
  leaf.page.body.page_generation = 1;
  leaf.page.body.free_space_offset = 0;  // SB_CATALOG_FREE_SPACE: unknown (encoded when tried)
  if (read_image) {
    leaf.image.assign(page_size, 0);
    const auto io = sink.ReadAt(taken.page_number * u64{page_size}, std::span<byte>(leaf.image));
    if (!io.ok() || io.bytes_transferred != page_size) {
      result.error = E::io_failure;
      result.io = io;
      return std::nullopt;
    }
  }
  return leaf;
}


// SEARCH_KEY: SB_CATALOG_LEAF_SPLIT (W3b-3a-2), SB_CATALOG_ROOT_LEAF_SPLIT
// (W3b-3b-1), SB_CATALOG_BRANCH_SPLIT (W3b-3b-2); owner Q30 = 1, Q45 = 1,
// Q46 = 1. Inserts `cell` into the leaf of `tree` whose fences hold its key,
// at its key's position; `changed` gains every page the insert changes. A
// full page splits; new pages are the system's (SB_NATIVE_BTREE_SYSTEM_PAGE).
//  - A leaf that is not the root: its cells and the new one divide at the
//    first split point both sides of which encode (all old cells left and the
//    new one alone right when it is last, the common case of ascending UUIDv7
//    keys; else the middle); the right part goes to a new leaf. The old leaf
//    keeps its page, low fence and left link and takes the split key as high
//    fence and the new leaf as right link; the new leaf takes the split key as
//    low fence and the old high fence and right link; the old right
//    neighbour's left link becomes the new leaf; the parent gains the
//    separator (the split key, the new leaf as child), which may split it.
//  - A root that is the only page keeps its page (page zero and the route
//    roots name its number and generation): its cells move to two new leaves
//    and it becomes a level-1 branch over them, the split key its separator.
//  - A full branch that is not the root splits at its middle separator: the
//    branch keeps its first child and the separators before it, a new branch
//    takes that separator's child as first child and the separators after it,
//    the middle key goes up as the separator for the new branch; fences and
//    sibling links as for a leaf. The moved children's stored parent
//    references are not rewritten: a page's parent is the page the walk
//    reaches it from (SB_CATALOG_PARENT_UNCHECKED, owner Q46 = 1).
//  - A full root branch keeps its page: its first child and separators move
//    to two new branches split at the middle separator, and it becomes a
//    branch one level higher over them.
// Write order (SB_CATALOG_SPLIT_LAG, FirebirdSQL's precedence), rank 4*level
// + 0 for a page a split adds, + 1 for a split or otherwise changed page, + 2
// for an old right neighbour; each rank durable before the next, so a page's
// parent (one level up) is always written after it. A root rewritten over new
// children is one switch. The tree's depth is bounded by the readers'
// NativeCatalogBtreeShape::kMaximumDepth; a cell no page can hold is
// `too_long`.
namespace {
int SplitRank(u16 level, int role) { return 4 * static_cast<int>(level) + role; }
struct TreeEdit {
  disk::CarefulWriteSink& sink;
  std::vector<TreePage>& tree;
  std::set<std::size_t>& changed;
  E full;
  PageSource& pages;
  u32 page_size;
  CatalogVersionWriteResult& result;
  static pg::NativeBtreeKeyView View(const pg::NativeBtreeKey& k) {
    return pg::NativeBtreeKeyView{std::span<const byte>(k.encoded_key), k.row_uuid, k.version_uuid};
  }
  std::optional<std::size_t> Find(const disk::NativePageReference& ref) const {
    for (std::size_t t = 0; t < tree.size(); ++t)
      if (tree[t].ref.page_number == ref.page_number) return t;
    return std::nullopt;
  }
  void Mark(std::size_t t, int rank) {
    tree[t].rank = std::max(tree[t].rank, rank);
    changed.insert(t);
  }
  // A new system page of `type` like `like`, owned by the tree's index.
  std::optional<E> NewPage(const pg::NativeBtreePage& like, u32 type, pg::NativeBtreePage& out, disk::NativePageReference& ref,
                           std::vector<byte>& image) {
    const auto taken = pages.Take(type, like.dependencies.index_uuid);
    if (!taken) return pages.disagreed() ? E::invalid_catalog : E::new_pages_exhausted;
    out = like;
    out.header.page_type = type;
    out.header.page_uuid = taken->page_uuid;
    out.header.page_number = taken->page_number;
    out.header.page_generation = 1;
    out.creator_transaction_uuid = {};
    out.creator_local_transaction_id = 0;
    out.parent.reset();
    out.left.reset();
    out.right.reset();
    out.low_fence.reset();
    out.high_fence.reset();
    out.first_child.reset();
    out.cells.clear();
    ref = {like.header.filespace_uuid, taken->page_number, 1, like.header.page_size_profile_uuid};
    if (!pages.dry()) {
      image.assign(page_size, 0);
      const auto io = sink.ReadAt(taken->page_number * u64{page_size}, std::span<byte>(image));
      if (!io.ok() || io.bytes_transferred != page_size) {
        result.io = io;
        return E::io_failure;
      }
    }
    return std::nullopt;
  }
  std::size_t Add(pg::NativeBtreePage page, const disk::NativePageReference& ref, std::vector<byte> image,
                   std::optional<std::size_t> parent_index) {
    TreePage added;
    added.ref = ref;
    added.page = std::move(page);
    added.image = std::move(image);
    added.added = true;
    added.parent_index = parent_index;
    // Rank 0 at its level, set outright: a new page must precede every page
    // that points to it (Mark only raises a rank).
    added.rank = SplitRank(added.page.tree_level, 0);
    tree.push_back(std::move(added));
    changed.insert(tree.size() - 1);
    return tree.size() - 1;
  }
  // The children of branch `t`: its first child, then each cell's child.
  std::vector<std::size_t> Children(std::size_t t) const {
    std::vector<std::size_t> out;
    const auto& page = tree[t].page;
    if (page.first_child)
      if (const auto c = Find(*page.first_child)) out.push_back(*c);
    for (const auto& c : page.cells)
      if (c.child)
        if (const auto i = Find(*c.child)) out.push_back(*i);
    return out;
  }
  void Adopt(std::size_t branch) {
    for (const auto c : Children(branch)) tree[c].parent_index = branch;
  }
  // Inserts the separator (key, child) into branch `at`, splitting it when full.
  std::optional<E> Separator(std::size_t at, const pg::NativeBtreeKey& key, const disk::NativePageReference& child,
                             std::size_t child_index) {
    auto page = tree[at].page;
    const auto level = page.tree_level;
    pg::NativeBtreeCell separator;
    separator.key = key;
    separator.child = child;
    const auto slot = std::lower_bound(page.cells.begin(), page.cells.end(), key,
                                       [&](const auto& c, const auto& k) { return pg::CompareNativeBtreeKeys(View(c.key), View(k)) < 0; });
    page.cells.insert(slot, std::move(separator));
    const auto fits = pg::EncodeNativeBtreePage(page);
    if (fits.ok()) {
      tree[at].page = std::move(page);
      tree[child_index].parent_index = at;
      Mark(at, SplitRank(level, 1));
      return std::nullopt;
    }
    if (fits.error != pg::NativeBtreeError::resource_exhausted) return E::invalid_catalog;
    const auto cells = page.cells;  // with the new separator
    if (cells.size() < 3) return full;
    const auto m = cells.size() / 2;
    // Left part: the first child and cells[0..m); right part: cells[m]'s child
    // as first child and cells(m..]; cells[m]'s key goes up.
    if (page.header.page_type == 0x200) {
      if (level + 2 > NativeCatalogBtreeShape::kMaximumDepth) return full;
      pg::NativeBtreePage left, right;
      disk::NativePageReference left_ref, right_ref;
      std::vector<byte> left_image, right_image;
      if (const auto e = NewPage(page, 0x201, left, left_ref, left_image)) return e;
      if (const auto e = NewPage(page, 0x201, right, right_ref, right_image)) return e;
      left.tree_level = right.tree_level = level;
      left.parent = right.parent = tree[at].ref;
      left.first_child = page.first_child;
      left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
      left.high_fence = cells[m].key;
      left.right = right_ref;
      right.first_child = cells[m].child;
      right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m) + 1, cells.end());
      right.low_fence = cells[m].key;
      right.left = left_ref;
      auto root = page;
      root.tree_level = static_cast<u16>(level + 1);
      root.first_child = left_ref;
      pg::NativeBtreeCell up;
      up.key = cells[m].key;
      up.child = right_ref;
      root.cells = {std::move(up)};
      if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(right).ok() || !pg::EncodeNativeBtreePage(root).ok())
        return full;
      const auto l = Add(std::move(left), left_ref, std::move(left_image), at);
      const auto r = Add(std::move(right), right_ref, std::move(right_image), at);
      tree[at].page = std::move(root);
      Mark(at, SplitRank(static_cast<u16>(level + 1), 1));
      Adopt(l);
      Adopt(r);
      return std::nullopt;
    }
    const auto parent = tree[at].parent_index;
    if (!parent) return E::invalid_catalog;
    const std::optional<std::size_t> neighbour = page.right ? Find(*page.right) : std::nullopt;
    if (page.right && !neighbour) return E::invalid_catalog;
    pg::NativeBtreePage right;
    disk::NativePageReference right_ref;
    std::vector<byte> right_image;
    if (const auto e = NewPage(page, 0x201, right, right_ref, right_image)) return e;
    right.tree_level = level;
    right.parent = page.parent;
    right.first_child = cells[m].child;
    right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m) + 1, cells.end());
    right.low_fence = cells[m].key;
    right.high_fence = page.high_fence;
    right.left = tree[at].ref;
    right.right = page.right;
    auto left = page;
    left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
    left.high_fence = cells[m].key;
    left.right = right_ref;
    if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(right).ok()) return full;
    const auto up_key = cells[m].key;
    const auto r = Add(std::move(right), right_ref, std::move(right_image), parent);
    tree[at].page = std::move(left);
    Mark(at, SplitRank(level, 1));
    if (neighbour) {
      tree[*neighbour].page.left = right_ref;
      Mark(*neighbour, SplitRank(level, 2));
    }
    Adopt(at);
    Adopt(r);
    return Separator(*parent, up_key, right_ref, r);
  }
};
}  // namespace

std::optional<E> InsertTreeCell(disk::CarefulWriteSink& sink, std::vector<TreePage>& tree, std::set<std::size_t>& changed,
                                pg::NativeBtreeCell cell, E full, E too_long, PageSource& pages, u32 page_size,
                                bool may_grow, CatalogVersionWriteResult& result) {
  TreeEdit edit{sink, tree, changed, full, pages, page_size, result};
  const auto view = TreeEdit::View;
  // SB_CATALOG_SPLIT_LAG (owner Q45 = 1): first complete any split a crash
  // interrupted: the parent's separator for the unlisted page (which may
  // split the parent), and the next page's left link.
  for (std::size_t t = 0; t < tree.size(); ++t) {
    if (!tree[t].unlisted) continue;
    if (!tree[t].page.low_fence || !tree[t].parent_index || !may_grow) return full;
    tree[t].unlisted = false;
    const auto low = *tree[t].page.low_fence;
    const auto ref = tree[t].ref;
    if (const auto right = tree[t].page.right)
      if (const auto next = edit.Find(*right); next && tree[*next].page.left != std::optional{ref}) {
        tree[*next].page.left = ref;
        edit.Mark(*next, SplitRank(tree[*next].page.tree_level, 2));
      }
    if (const auto e = edit.Separator(*tree[t].parent_index, low, ref, t)) return e;
  }
  std::optional<std::size_t> reached;
  for (std::size_t t = 0; t < tree.size() && !reached; ++t) {
    const auto& page = tree[t].page;
    if (page.tree_level) continue;
    if (page.low_fence && pg::CompareNativeBtreeKeys(view(cell.key), view(*page.low_fence)) < 0) continue;
    if (page.high_fence && pg::CompareNativeBtreeKeys(view(cell.key), view(*page.high_fence)) >= 0) continue;
    reached = t;
  }
  if (!reached) return E::invalid_catalog;
  auto cells = tree[*reached].page.cells;
  const auto at = std::lower_bound(cells.begin(), cells.end(), cell.key, [&](const auto& c, const auto& k) {
    return pg::CompareNativeBtreeKeys(view(c.key), view(k)) < 0;
  });
  const auto position = static_cast<std::size_t>(at - cells.begin());
  cells.insert(at, std::move(cell));
  {
    auto page = tree[*reached].page;
    page.cells = cells;
    const auto fits = pg::EncodeNativeBtreePage(page);
    if (fits.ok()) {
      tree[*reached].page = std::move(page);
      edit.Mark(*reached, SplitRank(0, 1));
      return std::nullopt;
    }
    if (fits.error == pg::NativeBtreeError::key_too_long) return too_long;
    if (fits.error != pg::NativeBtreeError::resource_exhausted) return E::invalid_catalog;
  }
  // The split: only where the write may add pages (an allocation map is bound).
  if (!may_grow) return full;
  const auto old = tree[*reached].page;
  std::vector<std::size_t> points;
  if (position + 1 == cells.size()) points.push_back(cells.size() - 1);
  points.push_back(cells.size() / 2);
  if (old.header.page_type == 0x200 && old.tree_level == 0) {
    // The root that is the only page: two new leaves, the root over them.
    pg::NativeBtreePage left, right;
    disk::NativePageReference left_ref, right_ref;
    std::vector<byte> left_image, right_image;
    if (const auto e = edit.NewPage(old, 0x202, left, left_ref, left_image)) return e;
    if (const auto e = edit.NewPage(old, 0x202, right, right_ref, right_image)) return e;
    for (const auto m : points) {
      if (m == 0 || m >= cells.size()) continue;
      auto l = left, r = right;
      l.parent = r.parent = tree[*reached].ref;
      l.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
      r.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m), cells.end());
      l.high_fence = cells[m].key;
      r.low_fence = cells[m].key;
      l.right = right_ref;
      r.left = left_ref;
      if (!pg::EncodeNativeBtreePage(l).ok() || !pg::EncodeNativeBtreePage(r).ok()) continue;
      auto root = old;
      root.tree_level = 1;
      root.first_child = left_ref;
      pg::NativeBtreeCell separator;
      separator.key = cells[m].key;
      separator.child = right_ref;
      root.cells = {std::move(separator)};
      if (!pg::EncodeNativeBtreePage(root).ok()) return E::invalid_catalog;
      const auto root_index = *reached;
      edit.Add(std::move(l), left_ref, std::move(left_image), root_index);
      edit.Add(std::move(r), right_ref, std::move(right_image), root_index);
      tree[root_index].page = std::move(root);
      edit.Mark(root_index, SplitRank(1, 1));
      return std::nullopt;
    }
    return full;
  }
  if (old.header.page_type != 0x202 || !tree[*reached].parent_index) return full;
  const auto parent = *tree[*reached].parent_index;
  const std::optional<std::size_t> neighbour = old.right ? edit.Find(*old.right) : std::nullopt;
  if (old.right && !neighbour) return E::invalid_catalog;
  pg::NativeBtreePage fresh;
  disk::NativePageReference new_ref;
  std::vector<byte> image;
  if (const auto e = edit.NewPage(old, 0x202, fresh, new_ref, image)) return e;
  for (const auto m : points) {
    if (m == 0 || m >= cells.size()) continue;
    auto left = old;
    left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
    left.right = new_ref;
    left.high_fence = cells[m].key;
    auto right = fresh;
    right.parent = old.parent;
    right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m), cells.end());
    right.left = tree[*reached].ref;
    right.right = old.right;
    right.low_fence = cells[m].key;
    right.high_fence = old.high_fence;
    if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(right).ok()) continue;
    const auto old_index = *reached;
    const auto split_key = cells[m].key;
    tree[old_index].page = std::move(left);
    edit.Mark(old_index, SplitRank(0, 1));
    const auto added = edit.Add(std::move(right), new_ref, std::move(image), parent);
    if (neighbour) {
      tree[*neighbour].page.left = new_ref;
      edit.Mark(*neighbour, SplitRank(0, 2));
    }
    return edit.Separator(parent, split_key, new_ref, added);
  }
  return full;
}

// Allocates the dry pass's pages, in order, each durable on its own (the
// system's: no creator). False with `result` set on a failure; the pages
// allocated before it are in result.allocated_pages.
bool AllocateNewPages(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                      std::span<const CatalogNewPageIdentity> identities, const std::vector<PageSource::Need>& needs,
                      CatalogVersionWriteResult& result) {
  if (!binding.allocation_root) {
    result.error = E::invalid_request;
    return false;
  }
  const pg::AllocationMapBinding map{binding.database_uuid, *binding.allocation_root};
  for (std::size_t i = 0; i < needs.size(); ++i) {
    pg::PageAllocationRequest request;
    request.page_type = needs[i].page_type;
    request.owner_uuid = needs[i].owner;
    request.page_uuid = identities[i].page_uuid;
    request.allocation_uuid = identities[i].allocation_uuid;
    const auto allocated = pg::AllocatePage(sink, map, request, binding.barrier_policy);
    result.writes += allocated.writes;
    result.syncs += allocated.syncs;
    if (!allocated.ok()) {
      using A = pg::AllocationWriteError;
      result.io = allocated.io;
      result.write_error = allocated.write_error;
      result.attempted = allocated.attempted || !result.allocated_pages.empty();
      result.error = allocated.error == A::filespace_full     ? E::filespace_full
                     : allocated.error == A::io_failure       ? E::io_failure
                     : allocated.error == A::invalid_map      ? E::invalid_catalog
                     : allocated.error == A::invalid_request  ? E::invalid_request
                     : allocated.attempted                    ? E::write_failed
                                                              : E::plan_refused;
      return false;
    }
    result.allocated_pages.push_back(allocated.page_number);
  }
  return true;
}
}  // namespace

const char* CatalogVersionWriteErrorName(CatalogVersionWriteError error) noexcept {
  switch (error) {
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::read_only_database: return "read_only_database";
    case E::io_failure: return "io_failure";
    case E::invalid_inventory: return "invalid_inventory";
    case E::invalid_catalog: return "invalid_catalog";
    case E::transaction_not_active: return "transaction_not_active";
    case E::read_only_transaction: return "read_only_transaction";
    case E::row_not_found: return "row_not_found";
    case E::write_conflict: return "write_conflict";
    case E::update_conflict: return "update_conflict";
    case E::row_deleted: return "row_deleted";
    case E::invalid_successor: return "invalid_successor";
    case E::ahead_of_catalog_root: return "ahead_of_catalog_root";
    case E::transaction_not_committed: return "transaction_not_committed";
    case E::root_regression: return "root_regression";
    case E::relation_full: return "relation_full";
    case E::duplicate_row: return "duplicate_row";
    case E::duplicate_object: return "duplicate_object";
    case E::invalid_insert: return "invalid_insert";
    case E::index_full: return "index_full";
    case E::invalid_role: return "invalid_role";
    case E::invalid_name: return "invalid_name";
    case E::duplicate_name: return "duplicate_name";
    case E::plan_refused: return "plan_refused";
    case E::write_failed: return "write_failed";
    case E::resource_exhausted: return "resource_exhausted";
    case E::name_tree_full: return "name_tree_full";
    case E::new_pages_exhausted: return "new_pages_exhausted";
    case E::filespace_full: return "filespace_full";
    case E::unaudited_change: return "unaudited_change";
    case E::duplicate_comment: return "duplicate_comment";
    case E::comment_tree_full: return "comment_tree_full";
    case E::missing_target: return "missing_target";
    case E::dependent_versions_exhausted: return "dependent_versions_exhausted";
    case E::tree_leaf_emptied: return "tree_leaf_emptied";
    case E::name_key_exceeds_page_capacity: return "name_key_exceeds_page_capacity";
    case E::object_last_name: return "object_last_name";
    case E::default_name_in_use: return "default_name_in_use";
    case E::duplicate_default_name: return "duplicate_default_name";
    case E::name_language_read_only: return "name_language_read_only";
    case E::definition_not_locked: return "definition_not_locked";
    case E::name_too_long: return "name_too_long";
    case E::name_contains_nul: return "name_contains_nul";
    case E::name_invalid_utf8: return "name_invalid_utf8";
    case E::name_null: return "name_null";
  }
  return "unknown";
}

namespace {
// ---- SB_NATIVE_NAME_CASCADE (N4b), shared by successors and, under owner
// Q48 = 1, inserts ----------------------------------------------------------
// A helper that stops the write returns the result to return; nullopt goes on.
using Stop = std::optional<CatalogVersionWriteResult>;
Stop Conflict(u64 blocker) {
  CatalogVersionWriteResult result;
  result.error = E::write_conflict;
  result.blocker_order = blocker;
  return result;
}

// SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): a catalog.metadata_change event row.
bool IsChangeEvent(const catalog::CatalogMetadataVersion& m) {
  return m.record.header.kind == catalog::CatalogRecordKind::audit_evidence &&
         catalog::IsCatalogMetadataChangePayload(m.record.payload);
}
// Whether `audit` names a catalog.metadata_change event stored in `events`
// (role 6) that this transaction wrote (its creator).
bool StoredOwnEvent(const Relation& events, const scratchbird::core::platform::TypedUuid& audit, const Uuid& transaction,
                    u64 order) {
  if (audit.kind != UuidKind::object) return false;
  for (const auto& leaf : events.leaves)
    for (const auto& row : leaf.page.body.rows) {
      const auto m = leaf.metadata.find(row.version_uuid);
      if (m == leaf.metadata.end() || m->second.record.header.object_uuid.value != audit.value) continue;
      if (IsChangeEvent(m->second) && !m->second.record.header.deleted && row.local_transaction_id == order &&
          row.transaction_uuid.value == transaction)
        return true;
    }
  return false;
}

// One new version of a row: a successor's own, or a dependent drop's.
// (Was also: an entry the name cascade moved, SB_NATIVE_NAME_CASCADE; retired
// by SB_NATIVE_NAME_ROW_V4.)
struct Succ {
  Uuid row, version;
  Version base;
  const catalog::CatalogMetadataVersion* base_metadata = nullptr;
  catalog::CatalogMetadataVersion metadata;
  std::optional<catalog::CatalogNamePayload> name;
  const catalog::CatalogNamePayload* base_name = nullptr;
  // SB_NATIVE_NAME_ROW_V4: a live entry's own-lane fold under its identifier
  // profile, decided by the entry check; its authoritative cell key is the
  // raw text so folded. (Was: its UUID-to-name display components.)
  std::optional<catalog::IdentifierFoldMode> own_fold;
  pg::RowDataRecord added;
  std::optional<std::size_t> target;
  // SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1): the base is this
  // transaction's own version of a name or comment row, and this version
  // replaces it in place (its slot, version UUID and row_version kept).
  bool replace = false;
  // SEARCH_KEY: SB_CATALOG_OWN_VERSION_MOVE (owner Q263 = 1, 2026-11-18): a
  // replacement that no longer fits its base's page is placed on another
  // page (a new base leaf if none has room) instead of being refused
  // relation_full; the base's row leaves its page (written last), the
  // version UUID and row_version are kept, and the version's cells in the
  // row-UUID tree (retargeted) and the comment tree (retargeted) point at the
  // new page. Only this transaction's own, uncommitted version moves;
  // committed versions are never touched. (Was: relation_full.)
  bool moved = false;
  // The row-UUID tree already held this version's cell and it was retargeted
  // (IndexNewLeafVersions then adds none).
  bool retargeted = false;
};
struct NameTree {
  std::vector<TreePage> tree;
  std::set<std::size_t> changed;
  // SB_CATALOG_OWN_VERSION_REPLACE: the (row, version) cells a replacement
  // removes; written first, as removals alone (BuildRemovalWrites).
  std::set<std::pair<Uuid, Uuid>> removed;
};

// SEARCH_KEY: SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1, 2026-11-13).
// Whether a successor of `base` (by this transaction, `order`) replaces it:
// a name or comment row whose base version is this transaction's own.
// kOwnVersionReplace is the switch the mutation check turns off.
inline constexpr bool kOwnVersionReplace = true;
bool ReplacesOwn(const catalog::CatalogMetadataVersion& base, const pg::RowDataRecord& base_row, u64 order) {
  const auto kind = base.record.header.kind;
  return kOwnVersionReplace && base_row.local_transaction_id == order &&
         (kind == catalog::CatalogRecordKind::localized_name || kind == catalog::CatalogRecordKind::localized_comment);
}
// Turns `c` into the replacement of its base: the base's version UUID,
// row_version, slot and links (the record is rewritten in its place).
void MakeReplacement(Succ& c, const pg::RowDataRecord& base_row) {
  c.replace = true;
  c.version = base_row.version_uuid;
  c.added = base_row;
  c.added.cells.clear();
}
// Removes the cells of (row, version) from the leaf level of `tree` as this
// write holds it, and records them for the removal writes. False when a leaf
// that is not the root would be left empty (the codec refuses an empty
// non-root leaf; there is no leaf merge).
bool RemoveCells(NameTree& tree, const Uuid& row, const Uuid& version) {
  tree.removed.insert({row, version});
  for (auto& page : tree.tree) {
    if (page.page.tree_level) continue;
    const auto before = page.page.cells.size();
    std::erase_if(page.page.cells, [&](const auto& cell) { return cell.key.row_uuid == row && cell.key.version_uuid == version; });
    if (page.page.cells.size() == before) continue;
    if (page.page.cells.empty() && page.parent_index) return false;
  }
  return true;
}
// The removal writes: every page of `tree` as read (not one this write adds)
// that holds a removed cell, re-encoded with those cells removed and nothing
// else changed. The page's "as read" image becomes that image, so the main
// plan's preimage is what the removal wrote. All are written before the main
// plan: a replaced version's old cells never outlive its old contents (the
// lease readers bind each version's cell count, and comment cells' keys).
struct RemovalWrite {
  u64 page_number = 0;
  std::vector<byte> image, as_read;
};
std::optional<E> BuildRemovalWrites(NameTree& tree, std::vector<RemovalWrite>& out) {
  if (tree.removed.empty()) return std::nullopt;
  for (auto& page : tree.tree) {
    if (page.added || page.page.tree_level) continue;
    auto read = pg::DecodeNativeBtreePage(page.image);
    if (!read.ok()) return E::invalid_catalog;
    const auto before = read.page->cells.size();
    std::erase_if(read.page->cells,
                  [&](const auto& cell) { return tree.removed.contains({cell.key.row_uuid, cell.key.version_uuid}); });
    if (read.page->cells.size() == before) continue;
    if (read.page->cells.empty() && page.parent_index) return E::tree_leaf_emptied;
    auto encoded = pg::EncodeNativeBtreePage(*read.page);
    if (!encoded.ok()) return E::invalid_catalog;
    out.push_back({page.ref.page_number, encoded.bytes, page.image});
    page.image = std::move(encoded.bytes);
  }
  return std::nullopt;
}
// Executes the removal writes as one plan, before the main one. False with
// `result` set when it fails.
bool ExecuteRemovals(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                     const std::vector<RemovalWrite>& writes, CatalogVersionWriteResult& result) {
  if (writes.empty()) return true;
  disk::CarefulWritePlan plan;
  for (const auto& w : writes) {
    const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(w.image), std::span<const byte>(w.as_read));
    if (!node.ok()) {
      result.error = E::plan_refused;
      return false;
    }
  }
  const auto r = plan.Execute(sink, binding.barrier_policy);
  result.writes += r.written;
  result.syncs += r.syncs;
  if (r.ok()) return true;
  result.write_error = r.error;
  result.io = r.io;
  result.attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                     r.error == CarefulWriteError::barrier_sync_failed || r.error == CarefulWriteError::final_sync_failed ||
                     r.error == CarefulWriteError::sink_exception;
  result.error = result.attempted                                      ? E::write_failed
                 : r.error == CarefulWriteError::preimage_read_failed ? E::io_failure
                 : r.error == CarefulWriteError::preimage_mismatch    ? E::invalid_catalog
                                                                      : E::plan_refused;
  return false;
}

// SB_NATIVE_NAME_TREES: the two name trees over the names relation, from page
// zero (kinds 22 and 23), read as the tree readers walk them: both or neither
// (a database without the roots - component fixtures, owner Q44 = 1 - has none
// to maintain or probe; one root without the other is a damaged catalog).
// `out` is left empty without them, else holds kind 22 then kind 23.
bool ReadNameTrees(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                   std::vector<byte>& image, std::vector<NameTree>& out, CatalogVersionWriteResult& result) {
  const auto& croot = binding.catalog_root;
  image.assign(page_size, 0);  // ReadPage refuses page zero: no catalog page is page zero
  const auto zero_io = sink.ReadAt(0, std::span<byte>(image));
  if (!zero_io.ok() || zero_io.bytes_transferred != page_size) {
    result.error = E::io_failure;
    result.io = zero_io;
    return false;
  }
  const disk::FilespaceBootstrapBinding zero_binding{binding.database_uuid, croot.filespace_uuid,
                                                     croot.page_size_profile_uuid};
  const auto zero = disk::DecodeFilespacePageZero(image.data(), image.size(), &zero_binding);
  if (!zero.ok()) {
    result.error = E::invalid_catalog;
    return false;
  }
  const auto& zero_roots = zero.record->roots;
  const auto find_root = [&](u16 kind) {
    return std::find_if(zero_roots.begin(), zero_roots.end(), [&](const auto& r) { return r.kind == kind; });
  };
  const auto first = find_root(22), second = find_root(23);
  if ((first == zero_roots.end()) != (second == zero_roots.end())) {
    result.error = E::invalid_catalog;
    return false;
  }
  if (first == zero_roots.end()) return true;
  out.resize(2);
  for (unsigned t = 0; t < 2; ++t) {
    const auto& r = t ? *second : *first;
    if (r.page_type != 0x200) {
      result.error = E::invalid_catalog;
      return false;
    }
    const pg::NativeCatalogRootReference head{
        2, 0x200, {r.filespace_uuid, r.page_number, r.page_generation, r.page_size_profile_uuid}, r.object_uuid};
    std::vector<disk::NativePageReference> bases;
    if (!ReadTree(sink, binding, page_size, head, out[t].tree, bases, result)) return false;
  }
  return true;
}

// SEARCH_KEY: SB_NATIVE_COMMENT_TREE (2026-11-13). The comments relation's
// (target, language) tree, from page zero (kind 24), read as the tree readers
// walk it. `out` stays empty without the root (a component fixture).
bool ReadCommentTree(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                     std::vector<byte>& image, std::optional<NameTree>& out, CatalogVersionWriteResult& result) {
  const auto& croot = binding.catalog_root;
  image.assign(page_size, 0);
  const auto zero_io = sink.ReadAt(0, std::span<byte>(image));
  if (!zero_io.ok() || zero_io.bytes_transferred != page_size) {
    result.error = E::io_failure;
    result.io = zero_io;
    return false;
  }
  const disk::FilespaceBootstrapBinding zero_binding{binding.database_uuid, croot.filespace_uuid,
                                                     croot.page_size_profile_uuid};
  const auto zero = disk::DecodeFilespacePageZero(image.data(), image.size(), &zero_binding);
  if (!zero.ok()) {
    result.error = E::invalid_catalog;
    return false;
  }
  const auto& zero_roots = zero.record->roots;
  const auto r = std::find_if(zero_roots.begin(), zero_roots.end(), [](const auto& z) { return z.kind == 24; });
  if (r == zero_roots.end()) return true;
  if (r->page_type != 0x200) {
    result.error = E::invalid_catalog;
    return false;
  }
  const pg::NativeCatalogRootReference head{pg::kNativeCommentRelationRole, 0x200,
                                            {r->filespace_uuid, r->page_number, r->page_generation,
                                             r->page_size_profile_uuid},
                                            r->object_uuid};
  out.emplace();
  std::vector<disk::NativePageReference> bases;
  return ReadTree(sink, binding, page_size, head, out->tree, bases, result);
}

// SEARCH_KEY: SB_NATIVE_COMMENT_RELATION (owner Q82 = 1). Whether a live
// comment of the comments relation holds (target, language), as this
// transaction sees it: per comment row, its newest version neither rolled back
// nor dead decides; committed or this transaction's own and not a drop is
// `held`; another transaction's in progress is a `blocker` (it may still
// commit the comment, or the drop). Candidates are the rows the tree's
// (target, language) range holds, or every row without the tree.
// SB_CATALOG_SINGLE_WRITER: holds only while no other catalog writer runs
// between the read and the write.
struct CommentHold {
  bool held = false, invalid = false;
  std::optional<u64> blocker;
};
CommentHold HoldOfCommentPair(const Relation& comments, const std::vector<TreePage>* tree, const Uuid& target,
                              const Uuid& language,
                              const std::function<const mga::TransactionInventoryEntry*(u64)>& entry, u64 order) {
  CommentHold out;
  const auto key = catalog::CatalogCommentTreeKey(target, language);
  std::set<Uuid> candidates;
  if (tree) {
    if (!ProbeTreePrefix(*tree, key, candidates)) {
      out.invalid = true;
      return out;
    }
  } else {
    for (const auto& leaf : comments.leaves)
      for (const auto& row : leaf.page.body.rows) candidates.insert(row.row_uuid.value);
  }
  for (const auto& id : candidates) {
    const pg::RowDataRecord* newest = nullptr;
    const catalog::CatalogMetadataVersion* newest_metadata = nullptr;
    std::optional<mga::TransactionState> newest_state;
    for (const auto& leaf : comments.leaves)
      for (const auto& row : leaf.page.body.rows) {
        if (row.row_uuid.value != id) continue;
        const auto* creator = entry(row.local_transaction_id);
        const auto m = leaf.metadata.find(row.version_uuid);
        if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value ||
            m == leaf.metadata.end()) {
          out.invalid = true;
          return out;
        }
        const auto state = mga::InventoryVisibilityState(*creator);
        if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal) continue;
        if (!newest || row.row_version > newest->row_version) {
          newest = &row;
          newest_metadata = &m->second;
          newest_state = state;
        }
      }
    if (!newest) continue;  // every version rolled back or dead: the pair is free
    const auto decoded = catalog::DecodeCatalogComment(newest_metadata->record.payload);
    if (!decoded.ok()) {
      out.invalid = true;
      return out;
    }
    if (decoded.record->target_object_uuid != target || decoded.record->language_uuid != language) {
      if (tree) {  // a cell keyed for this pair on a row that is not
        out.invalid = true;
        return out;
      }
      continue;
    }
    if (InProgress(*newest_state) && newest->local_transaction_id != order) {
      if (!out.blocker) out.blocker = newest->local_transaction_id;
      continue;
    }
    if (!newest_metadata->record.header.deleted) out.held = true;
  }
  return out;
}

// SEARCH_KEY: SB_NATIVE_NAME_CASCADE (N4b), since SB_NATIVE_NAME_ROW_V4
// (owner Q157 = 1, Q158 = 1, 2026-11-14; plan 35 S2): a change to a name
// (rename, drop, a default_for_language or vector default change, an insert
// that adds a language default) writes only its own rows. No entry stores its
// path or its path's display, so nothing below the changed name is rewritten.
// What N4b checked besides the regeneration is kept, in this transaction's
// own view (owner Q226, 2026-11-15, SB_NATIVE_NAME_CONFLICT_WRITE_WRITE_ONLY):
// every entry this transaction sees (the version of its row it sees:
// committed, or its own; not deleted, dropped or archived; `skip`: the rows
// the request itself writes) whose path no longer walks over `after` (the
// names after the change: an unnamed or ambiguous scope) is `unresolvable`,
// refused before any write: its path is walked at read and must walk.
// The version seen is used even when another transaction has a newer version
// of the row in progress (that version is not seen until it commits).
// (Was, Q212, SB_NATIVE_NAME_PATH_CONFLICT_BREAK: also another transaction's
// entry in progress conflicted with the change - write_conflict - when its
// path walked in one ending of this transaction and not the other, walked
// without and with the change; the "seen" check took only a row whose newest
// version was committed or this transaction's, so a committed entry with
// another transaction's successor in progress was left to that conflict.
// Removed by owner Q226: names conflict only write-write on the same row or
// by name-key uniqueness.)
// (Was, Q47 = 1: such an entry conflicted when a changed row decided a lookup
// on its path and the path did not resolve before or after the change, or
// when the change moved it: a component chosen differently, or a chosen
// component's text or lane inputs changed (PathMoved), so a rename of a scope
// conflicted with every entry in progress below it.)
// (Was: also each entry this transaction sees whose stored full path, count
// or UUID-to-name display components the change moved got a successor version
// in the same write, from the request's cascade_versions (too few:
// cascade_versions_exhausted), after a PendingOnPath check of its new path;
// FindMoved, BuildCascade.)
// (Was: PathMoved, whether two resolved paths differed in the component
// chosen, its raw text or its lane inputs; retired by owner Q212.)
Stop CheckPathsAfterChange(const NameState& names, const catalog::CatalogNameIndex& after, std::span<const Uuid> skip,
                           E unresolvable) {
  const auto stored = names.Stored();
  for (const auto& [row, k] : names.Visible()) {
    if (std::find(skip.begin(), skip.end(), row) != skip.end()) continue;
    const auto& s = stored[k];
    if (s.deleted) continue;
    const auto* e = std::get_if<catalog::CatalogNameEntry>(&s.payload);
    if (!e || e->lifecycle_state == catalog::CatalogNameLifecycle::dropped ||
        e->lifecycle_state == catalog::CatalogNameLifecycle::archived)
      continue;
    if (catalog::CheckCatalogNameEntryPath(catalog::BorrowCatalogNameEntry(*e), after) != catalog::CatalogNamePathError::none)
      return Refuse(unresolvable);
  }
  return std::nullopt;
}

// SEARCH_KEY: SB_NATIVE_NAME_DEFINITION_LOCK (owner Q247 = yes,
// 2026-11-16). Whether this transaction has written a version of a
// definition row carrying `object` (any relation but the names and the
// comments): its "touch" (a successor changing nothing else), its drop, or
// its insert.
bool OwnDefinitionVersion(const std::map<u16, Relation>& relations, const Uuid& object, u64 order) {
  for (const auto& [role, relation] : relations) {
    if (role == 2 || role == pg::kNativeCommentRelationRole) continue;
    for (const auto& leaf : relation.leaves)
      for (const auto& row : leaf.page.body.rows) {
        if (row.local_transaction_id != order) continue;
        const auto m = leaf.metadata.find(row.version_uuid);
        if (m == leaf.metadata.end()) continue;
        const auto& h = m->second.record.header;
        if (h.object_uuid.value == object && h.kind != catalog::CatalogRecordKind::localized_name &&
            h.kind != catalog::CatalogRecordKind::localized_comment)
          return true;
      }
  }
  return false;
}
// The name list rules for a name row's successor, in this transaction's own
// view (each name row's version it sees: committed, or its own;
// NameState::Visible). In order:
//  - SB_NATIVE_NAME_LANGUAGE_READ_ONLY (owner Q250): an entry's language
//    never changes: name_language_read_only;
//  - a vector removed (deleted, lifecycle dropped or archived):
//    object_last_name (only the object's drop removes it, Q249);
//  - SB_NATIVE_NAME_LAST_NAME (Q227): an entry removed when the object has no
//    other live entry: object_last_name;
//  - SB_NATIVE_NAME_DEFAULT_IN_USE (Q235 = 1): an entry removed whose version
//    seen is marked default_for_language or default_for_object:
//    default_name_in_use;
//  - SB_NATIVE_NAME_DEFAULT_INDEX (Q228 revisited): an entry kept whose flags
//    mark a default: NameState::CheckDefault;
//  - SB_NATIVE_NAME_DEFINITION_LOCK (Q247): an entry removed, or its default
//    flags changed, needs this transaction's version of the object's
//    definition row (definition_not_locked). `read_definitions` reads the
//    definition relations into `relations` when needed.
// Another transaction's in-progress versions are consulted only by the
// default index (unique-index semantics); the rest is write-write on rows
// (owner Q226, Q247).
// `other` (SB_NATIVE_NAME_DEFAULT_SWITCH): the paired row of a default switch
// and its new version; the default index sees it in place of that row's
// stored versions, and default_for_object may be cleared only when it sets
// it (else default_name_in_use: the object would lose its default).
Stop CheckNameListChange(const NameState& names, const Uuid& row, const catalog::CatalogNamePayload& before,
                         const catalog::CatalogNamePayload& after, bool deleted, u64 order,
                         const std::map<u16, Relation>& relations, const std::function<Stop()>& read_definitions,
                         const std::pair<Uuid, const catalog::CatalogNameEntry*>* other = nullptr) {
  const auto stored = names.Stored();
  const auto removes = [](const catalog::CatalogNamePayload& p, bool d) {
    const auto state = std::visit([](const auto& r) { return r.lifecycle_state; }, p);
    return d || state == catalog::CatalogNameLifecycle::dropped || state == catalog::CatalogNameLifecycle::archived;
  };
  if (std::holds_alternative<catalog::CatalogNameVector>(after)) {
    if (removes(after, deleted)) return Refuse(E::object_last_name);
    return std::nullopt;
  }
  const auto& e = std::get<catalog::CatalogNameEntry>(after);
  const auto& b = std::get<catalog::CatalogNameEntry>(before);
  if (e.language_uuid.value != b.language_uuid.value) return Refuse(E::name_language_read_only);
  const bool removed = removes(after, deleted);
  if (removed) {
    std::size_t others = 0;
    for (const auto& [r, k] : names.Visible()) {
      if (r == row) continue;
      const auto* o = std::get_if<catalog::CatalogNameEntry>(&stored[k].payload);
      if (o && o->object_uuid.value == e.object_uuid.value && NameState::LiveName(stored[k])) ++others;
    }
    if (!others) return Refuse(E::object_last_name);
    if (b.default_for_language || b.default_for_object) return Refuse(E::default_name_in_use);
  } else {
    if (b.default_for_object && !e.default_for_object &&
        !(other && other->second && other->second->default_for_object &&
          other->second->object_uuid.value == e.object_uuid.value))
      return Refuse(E::default_name_in_use);
    const auto verdict = names.CheckDefault(e, row, other);
    if (verdict.error != E::none) {
      auto refused = Refuse(verdict.error);
      refused.blocker_order = verdict.blocker;
      return refused;
    }
  }
  if (removed || e.default_for_language != b.default_for_language || e.default_for_object != b.default_for_object) {
    if (auto stop = read_definitions()) return stop;
    if (!OwnDefinitionVersion(relations, e.object_uuid.value, order)) return Refuse(E::definition_not_locked);
  }
  return std::nullopt;
}

// A version's metadata as placed on `page` at `slot`: a name's envelope binds
// where it is (SB_NATIVE_NAME_TREES), and must then follow the base.
std::optional<std::vector<byte>> PlacedImage(const Succ& c, const NativeCatalogLeafPage& page, u32 slot,
                                             const Uuid& transaction, u64 order) {
  auto final_metadata = c.metadata;
  if (c.name) {
    const catalog::CatalogNameVersionBinding at{
        {UuidKind::database, page.header.database_uuid}, {UuidKind::filespace, page.header.filespace_uuid},
        {UuidKind::row, c.row}, {UuidKind::row, c.version}, c.metadata.record.header.object_uuid,
        {UuidKind::transaction, transaction}, page.body.page_number, slot, page.header.page_generation,
        c.added.row_version, order, c.metadata.catalog_generation};
    const auto envelope = catalog::EncodeCatalogNameEnvelope({at, *c.name});
    if (!envelope.ok()) return std::nullopt;
    final_metadata.record.payload.assign(envelope.bytes.begin(), envelope.bytes.end());
    if (!FollowsAs(*c.base_metadata, final_metadata, c.row, transaction, order)) return std::nullopt;
  }
  // SB_CATALOG_OWN_VERSION_REPLACE: checked as the successor of the replaced
  // version (definition_version one more); written in its place with its
  // definition_version (one more than its committed predecessor's, as the
  // visibility reader checks a resident chain).
  if (c.replace) final_metadata.definition_version = c.base_metadata->definition_version;
  const auto encoded = catalog::EncodeCatalogMetadataVersion(final_metadata);
  if (!encoded.ok()) return std::nullopt;
  return encoded.bytes;
}

// Placement: every page is edited in memory (`work`, the relation's leaf
// pages as this write has them so far) and written once. A version goes on
// its predecessor's page when it has room (the version and its forward link,
// a hint to readers, in one image); otherwise on a page that carries no
// forward link in this write, or on a new base leaf. `link_pages` gets the
// pages carrying links: the caller writes every other page first, so no link
// is ever written before its version and no two pages need each other first.
Stop PlaceSuccs(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, std::vector<Succ>& succs,
                Relation& relation, const pg::NativeCatalogRootReference& head, std::vector<NativeCatalogLeafPage>& work,
                std::set<std::size_t>& link_pages, PageSource& pages, u32 page_size, const Uuid& transaction, u64 order) {
  for (const auto& c : succs)
    if (!c.replace) link_pages.insert(c.base.leaf);
  std::optional<E> place_error;
  const auto try_place = [&](Succ& c, std::size_t i) -> bool {
    const auto& candidate = work[i];
    u32 slot = 0;
    for (const auto& row : candidate.body.rows) slot = std::max(slot, row.stable_slot_id);
    if (slot == std::numeric_limits<u32>::max()) return false;
    auto row = c.added;
    row.stable_slot_id = slot + 1;
    row.storage_generation = candidate.header.page_generation;
    auto payload = PlacedImage(c, candidate, slot + 1, transaction, order);
    if (!payload) {
      place_error = E::invalid_successor;
      return false;
    }
    scratchbird::core::datatypes::DatatypeBinaryValue cell;
    cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
    cell.payload = std::move(*payload);
    row.cells = {{1, std::move(cell), {}}};
    // SB_CATALOG_FREE_SPACE: a page the row cannot fit is not copied or
    // encoded; the page it may fit is, and the encoder decides.
    if (RowFits(candidate, row) == false) return false;
    auto page = candidate;
    page.body.rows.push_back(std::move(row));
    const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
    if (!fits.ok()) {
      if (fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE") return false;
      place_error = fits.resource_failure() ? E::resource_exhausted : E::invalid_successor;
      return false;
    }
    page.body.free_space_offset = fits.body.free_space_offset;  // SB_CATALOG_FREE_SPACE
    work[i] = std::move(page);
    c.target = i;
    return true;
  };
  // SB_CATALOG_OWN_VERSION_REPLACE (Q88 = 1): a replacement takes its
  // base's record, in its slot, on its page (one whole-page write).
  // SB_CATALOG_OWN_VERSION_MOVE (owner Q263 = 1, 2026-11-18): one that does
  // not fit there is moved: placed below like any version without a page,
  // its base's row removed from the base page, which is written after the
  // page that now holds the version (so a fault between the writes leaves
  // this transaction's uncommitted version on two pages, never on none).
  // (Was: refused relation_full, "moving it would leave its version UUID on
  // two pages between the writes".)
  for (auto& c : succs) {
    if (!c.replace) continue;
    auto page = work[c.base.leaf];
    auto row = c.added;
    auto payload = PlacedImage(c, page, row.stable_slot_id, transaction, order);
    if (!payload) return Refuse(E::invalid_successor);
    scratchbird::core::datatypes::DatatypeBinaryValue cell;
    cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
    cell.payload = std::move(*payload);
    row.cells = {{1, std::move(cell), {}}};
    page.body.rows[c.base.row] = std::move(row);
    const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
    if (!fits.ok()) {
      if (fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE") {
        c.moved = true;
        continue;
      }
      return Refuse(fits.resource_failure() ? E::resource_exhausted : E::invalid_successor);
    }
    page.body.free_space_offset = fits.body.free_space_offset;  // SB_CATALOG_FREE_SPACE
    work[c.base.leaf] = std::move(page);
    c.target = c.base.leaf;
  }
  for (auto& c : succs) {
    if (c.replace) continue;
    auto& row = work[c.base.leaf].body.rows[c.base.row];
    row.next_version_uuid = c.version;
    row.next_row_version = c.added.row_version;
    if (try_place(c, c.base.leaf)) continue;
    if (place_error) return Refuse(*place_error);
  }
  // SB_CATALOG_OWN_VERSION_MOVE: the moved replacements' base rows leave
  // their pages (highest row index first, so the indices of the others on
  // the same page stay valid; nothing below indexes `work` by base.row), and
  // those pages are written last (link_pages). Each moved version is then
  // placed by the loop below: on a page with room, else on a new base leaf.
  {
    std::vector<Succ*> moved;
    for (auto& c : succs)
      if (c.moved) moved.push_back(&c);
    std::sort(moved.begin(), moved.end(), [](const Succ* a, const Succ* b) {
      return a->base.leaf != b->base.leaf ? a->base.leaf < b->base.leaf : a->base.row > b->base.row;
    });
    for (auto* c : moved) {
      auto& page = work[c->base.leaf];
      if (c->base.row >= page.body.rows.size() || page.body.rows[c->base.row].version_uuid != c->version)
        return Refuse(E::invalid_successor);
      page.body.rows.erase(page.body.rows.begin() + static_cast<std::ptrdiff_t>(c->base.row));
      const auto rebuilt = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
      if (!rebuilt.ok()) return Refuse(rebuilt.resource_failure() ? E::resource_exhausted : E::invalid_successor);
      page.body.free_space_offset = rebuilt.body.free_space_offset;  // SB_CATALOG_FREE_SPACE
      link_pages.insert(c->base.leaf);
    }
  }
  for (auto& c : succs) {
    if (c.target) continue;
    for (std::size_t i = 0; i < work.size() && !c.target; ++i) {
      if (link_pages.contains(i)) continue;
      if (try_place(c, i)) break;
      if (place_error) return Refuse(*place_error);
    }
    if (c.target) continue;
    // SB_CATALOG_NEW_PAGES (W3b-3a): a new base leaf of an indexed relation.
    if (head.page_type != 0x200 || !binding.allocation_root || relation.leaves.empty()) return Refuse(E::relation_full);
    const auto& like = relation.leaves.front().page;
    const auto taken = pages.Take(6, like.body.relation_uuid.value);
    if (!taken) return Refuse(pages.disagreed() ? E::invalid_catalog : E::new_pages_exhausted);
    CatalogVersionWriteResult result;
    auto leaf = NewBaseLeaf(sink, like, page_size, *taken, !pages.dry(), result);
    if (!leaf) return result;
    work.push_back(leaf->page);
    relation.leaves.push_back(std::move(*leaf));
    relation.bases.push_back({binding.catalog_root.filespace_uuid, taken->page_number, 1,
                              binding.catalog_root.page_size_profile_uuid});
    if (!try_place(c, work.size() - 1)) return Refuse(place_error ? *place_error : E::relation_full);
  }
  return std::nullopt;
}

// SB_CATALOG_OWN_VERSION_MOVE (owner Q263 = 1, 2026-11-18): a moved
// replacement's cell in the row-UUID tree, when the tree holds one (the row's
// first version, or a version a new leaf was added for), now names the page
// the version moved to. Changed in place (the key is unchanged, so the cell's
// size is); the page is written with the other changed tree pages, after the
// base leaves. A moved version without a cell gets none here (it is reached
// as any later version is); IndexNewLeafVersions adds one when it moved onto
// a leaf this write added and it had none.
Stop RetargetMovedRowCells(std::vector<Succ>& succs, Relation& relation, std::set<std::size_t>& changed) {
  for (auto& c : succs) {
    if (!c.moved) continue;
    for (std::size_t p = 0; p < relation.tree.size() && !c.retargeted; ++p) {
      auto& page = relation.tree[p];
      if (page.page.tree_level) continue;
      for (auto& cell : page.page.cells) {
        if (cell.key.row_uuid != c.row || cell.key.version_uuid != c.version) continue;
        auto changed_page = page.page;
        changed_page.cells[&cell - page.page.cells.data()].base_page = relation.bases[*c.target];
        const auto fits = pg::EncodeNativeBtreePage(changed_page);
        if (!fits.ok()) return Refuse(fits.error == pg::NativeBtreeError::resource_exhausted ? E::resource_exhausted : E::invalid_catalog);
        page.page = std::move(changed_page);
        changed.insert(p);
        c.retargeted = true;
        break;
      }
    }
  }
  return std::nullopt;
}

// SB_CATALOG_NEW_PAGES: a version on a leaf this write added is reached by its
// own cell in the row-UUID tree (the tree otherwise indexes first versions
// only; the visibility reader selects one visible version per row however
// many of its versions have cells). Leaves from `first_new` on were added.
Stop IndexNewLeafVersions(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                          const std::vector<Succ>& succs, Relation& relation, std::size_t first_new,
                          std::set<std::size_t>& changed, PageSource& pages, u32 page_size, u64 order) {
  for (const auto& c : succs) {
    if (*c.target < first_new) continue;
    if (c.retargeted) continue;  // SB_CATALOG_OWN_VERSION_MOVE: its existing cell was retargeted
    pg::NativeBtreeCell cell;
    cell.key.encoded_key.assign(c.row.bytes.begin(), c.row.bytes.end());
    cell.key.row_uuid = c.row;
    cell.key.version_uuid = c.version;
    cell.base_page = relation.bases[*c.target];
    cell.creator_local_transaction_id = order;
    CatalogVersionWriteResult result;
    if (const auto error = InsertTreeCell(sink, relation.tree, changed, std::move(cell), E::index_full,
                                          E::invalid_successor, pages, page_size, binding.allocation_root.has_value(),
                                          result))
      return Refuse(*error);
  }
  return std::nullopt;
}

// The name trees (SB_NATIVE_NAME_TREES, owner Q42): each base version's cells
// are stamped with this transaction; a version that is not a drop gets its own
// cells (`trees`: the authoritative tree, then the UUID-to-name tree).
Stop StampAndAddNameCells(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                          const std::vector<Succ>& succs, const Relation& relation, NameTree* trees,
                          const std::function<const mga::TransactionInventoryEntry*(u64)>& entry, PageSource& pages,
                          u32 page_size, u64 order) {
  for (const auto& c : succs) {
    const auto& c_base_row = relation.leaves[c.base.leaf].page.body.rows[c.base.row];
    const bool base_is_entry = std::holds_alternative<catalog::CatalogNameEntry>(*c.base_name);
    // The stamps: the base version's cell in each tree that holds one. A
    // replacement (SB_CATALOG_OWN_VERSION_REPLACE) stamps nothing: its base's
    // cells were removed (RemoveReplacedNameCells) and its own are added.
    for (unsigned t = 0; t < 2; ++t) {
      if (c.replace) break;
      if (t == 0 && !base_is_entry) continue;
      bool stamped = false;
      for (std::size_t p = 0; p < trees[t].tree.size() && !stamped; ++p) {
        auto& page = trees[t].tree[p];
        if (page.page.tree_level) continue;
        for (auto& cell : page.page.cells) {
          if (cell.key.version_uuid != c_base_row.version_uuid || cell.key.row_uuid != c.row) continue;
          if (cell.superseding_local_transaction_id && cell.superseding_local_transaction_id != order) {
            // A stamp already there: from a successor that rolled back or
            // died it is void and is overwritten; from one still in progress
            // it is a write conflict; from one that committed the base would
            // not be the newest version.
            const auto* stamper = entry(cell.superseding_local_transaction_id);
            const auto state = stamper ? std::optional{mga::InventoryVisibilityState(*stamper)} : std::nullopt;
            if (!state || *state == mga::TransactionState::committed) return Refuse(E::invalid_catalog);
            if (InProgress(*state)) return Conflict(cell.superseding_local_transaction_id);
          }
          auto changed = page.page;
          changed.cells[&cell - page.page.cells.data()].superseding_local_transaction_id = order;
          const auto fits = pg::EncodeNativeBtreePage(changed);
          if (!fits.ok())
            return Refuse(fits.error == pg::NativeBtreeError::resource_exhausted ? E::name_tree_full : E::invalid_catalog);
          page.page = std::move(changed);
          trees[t].changed.insert(p);
          stamped = true;
          break;
        }
      }
      if (!stamped) return Refuse(E::invalid_catalog);  // a name row version without its cell
    }
    // The new version's cells, unless it is a drop.
    if (c.metadata.record.header.deleted) continue;
    const auto payload_view = catalog::BorrowCatalogNamePayload(*c.name);
    CatalogVersionWriteResult result;
    const auto add = [&](unsigned t, std::vector<byte> encoded_key, std::vector<byte> payload) -> std::optional<E> {
      pg::NativeBtreeCell cell;
      cell.key.encoded_key = std::move(encoded_key);
      cell.key.row_uuid = c.row;
      cell.key.version_uuid = c.version;
      cell.base_page = relation.bases[*c.target];
      cell.creator_local_transaction_id = order;
      cell.payload = std::move(payload);
      // SB_NATIVE_NAME_KEY_EXCEEDS_PAGE_CAPACITY (owner Q225 = yes): a cell
      // key too long for the page has its own code (was: invalid_successor).
      return InsertTreeCell(sink, trees[t].tree, trees[t].changed, std::move(cell), E::name_tree_full,
                            E::name_key_exceeds_page_capacity, pages, page_size, binding.allocation_root.has_value(),
                            result);
    };
    if (const auto* v = std::get_if<catalog::CatalogNameVectorView>(&payload_view)) {
      const auto kind = KnownKind(v->object_kind);
      if (!kind) return Refuse(E::invalid_name);
      if (const auto error = add(1, catalog::CatalogNameUuidTreeVectorKey(v->object_uuid.value, *kind),
                                 catalog::CatalogNameUuidTreeVectorPayload(*v)))
        return Refuse(*error);
    } else {
      const auto& e = std::get<catalog::CatalogNameEntryView>(payload_view);
      const auto kind = KnownKind(e.object_kind);
      if (!kind || !c.own_fold) return Refuse(E::invalid_name);
      const catalog::CatalogNameDerivedText own{e.raw_name_text, *c.own_fold};
      if (const auto error = add(0, catalog::CatalogNameAuthoritativeTreeKey(e, own, *kind),
                                 catalog::CatalogNameAuthoritativeTreePayload(e)))
        return Refuse(*error);
      if (const auto error = add(1, catalog::CatalogNameUuidTreeEntryKey(e, *kind),
                                 catalog::CatalogNameUuidTreeEntryPayload(e)))
        return Refuse(*error);
    }
  }
  return std::nullopt;
}
// SB_NATIVE_COMMENT_TREE: a successor's comment cells, as the name trees'
// (owner Q42): the base version's cell is stamped with this transaction (a
// stamp from another transaction in progress is a write conflict; from one
// rolled back or dead it is void and overwritten); a version that is not a
// drop gets its own cell under the same (target, language).
Stop StampAndAddCommentCell(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, const Succ& c,
                            const Relation& relation, NameTree& tree,
                            const std::function<const mga::TransactionInventoryEntry*(u64)>& entry, PageSource& pages,
                            u32 page_size, u64 order, const catalog::CatalogCommentRecordView& comment) {
  // SB_CATALOG_OWN_VERSION_REPLACE: a replacement's cell is its base's: kept
  // for an edit (its key cannot change), removed for a drop
  // (RemoveReplacedCommentCell). SB_CATALOG_OWN_VERSION_MOVE (Q263 = 1): a
  // kept cell of a replacement that moved pages now names the new page.
  if (c.replace) {
    if (!c.moved || c.metadata.record.header.deleted) return std::nullopt;
    for (std::size_t p = 0; p < tree.tree.size(); ++p) {
      auto& page = tree.tree[p];
      if (page.page.tree_level) continue;
      for (auto& cell : page.page.cells) {
        if (cell.key.version_uuid != c.version || cell.key.row_uuid != c.row) continue;
        auto changed = page.page;
        changed.cells[&cell - page.page.cells.data()].base_page = relation.bases[*c.target];
        const auto fits = pg::EncodeNativeBtreePage(changed);
        if (!fits.ok())
          return Refuse(fits.error == pg::NativeBtreeError::resource_exhausted ? E::comment_tree_full : E::invalid_catalog);
        page.page = std::move(changed);
        tree.changed.insert(p);
        return std::nullopt;
      }
    }
    return Refuse(E::invalid_catalog);  // a moved comment version without its cell
  }
  const auto& base_row = relation.leaves[c.base.leaf].page.body.rows[c.base.row];
  bool stamped = false;
  for (std::size_t p = 0; p < tree.tree.size() && !stamped; ++p) {
    auto& page = tree.tree[p];
    if (page.page.tree_level) continue;
    for (auto& cell : page.page.cells) {
      if (cell.key.version_uuid != base_row.version_uuid || cell.key.row_uuid != c.row) continue;
      if (cell.superseding_local_transaction_id && cell.superseding_local_transaction_id != order) {
        const auto* stamper = entry(cell.superseding_local_transaction_id);
        const auto state = stamper ? std::optional{mga::InventoryVisibilityState(*stamper)} : std::nullopt;
        if (!state || *state == mga::TransactionState::committed) return Refuse(E::invalid_catalog);
        if (InProgress(*state)) return Conflict(cell.superseding_local_transaction_id);
      }
      auto changed = page.page;
      changed.cells[&cell - page.page.cells.data()].superseding_local_transaction_id = order;
      const auto fits = pg::EncodeNativeBtreePage(changed);
      if (!fits.ok())
        return Refuse(fits.error == pg::NativeBtreeError::resource_exhausted ? E::comment_tree_full : E::invalid_catalog);
      page.page = std::move(changed);
      tree.changed.insert(p);
      stamped = true;
      break;
    }
  }
  if (!stamped) return Refuse(E::invalid_catalog);  // a comment version without its cell
  if (c.metadata.record.header.deleted) return std::nullopt;
  const auto key = catalog::CatalogCommentTreeKey(comment.target_object_uuid, comment.language_uuid);
  pg::NativeBtreeCell cell;
  cell.key.encoded_key.assign(key.begin(), key.end());
  cell.key.row_uuid = c.row;
  cell.key.version_uuid = c.version;
  cell.base_page = relation.bases[*c.target];
  cell.creator_local_transaction_id = order;
  CatalogVersionWriteResult result;
  if (const auto error = InsertTreeCell(sink, tree.tree, tree.changed, std::move(cell), E::comment_tree_full,
                                        E::invalid_successor, pages, page_size, binding.allocation_root.has_value(),
                                        result))
    return Refuse(*error);
  return std::nullopt;
}
// SB_CATALOG_OWN_VERSION_REPLACE: a name replacement's base cells come out of
// both trees (its own, from its new contents, go in with the additions); a
// comment replacement's base cell comes out when it is a drop.
Stop RemoveReplacedNameCells(const std::vector<Succ>& succs, std::vector<NameTree>& trees) {
  for (const auto& c : succs)
    if (c.replace)
      for (auto& tree : trees)
        if (!RemoveCells(tree, c.row, c.version)) return Refuse(E::tree_leaf_emptied);
  return std::nullopt;
}
Stop RemoveReplacedCommentCell(const Succ& c, NameTree& tree) {
  if (c.replace && c.metadata.record.header.deleted && !RemoveCells(tree, c.row, c.version))
    return Refuse(E::tree_leaf_emptied);
  return std::nullopt;
}

// ---- SB_NATIVE_COMMENT_TARGET (owner 2026-11-13, Q86, Q87 = 1) -------------
// Every relation of the catalog root a comment's target may live in (all but
// the names, role 2, and the comments, role 7) read into `relations`, those
// already there kept. False with `result` set on a refusal.
bool ReadDefinitionRelations(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                             const pg::NativeCatalogRoot& root, std::map<u16, Relation>& relations,
                             std::map<u16, pg::NativeCatalogRootReference>* heads, CatalogVersionWriteResult& result) {
  for (const auto& head : root.roots) {
    if (head.role == 2 || head.role == pg::kNativeCommentRelationRole || relations.contains(head.role)) continue;
    if (head.page_type != 6 && head.page_type != 0x200) {
      result.error = E::invalid_catalog;
      return false;
    }
    if (heads) (*heads)[head.role] = head;
    if (!ReadRelation(sink, binding, page_size, head, relations[head.role], result)) return false;
  }
  return true;
}
// Whether `target` is a live definition in this transaction's view
// (SB_NATIVE_COMMENT_TARGET in the header): per row of a definition relation
// carrying it as its object, newest version first, rolled-back and dead
// versions passed over. `in_request`: a row of this insert carries it.
struct TargetVerdict {
  E error = E::none;
  std::optional<u64> blocker;
};
TargetVerdict CheckCommentTarget(const std::map<u16, Relation>& relations, const Uuid& target,
                                 const std::function<const mga::TransactionInventoryEntry*(u64)>& entry, u64 order,
                                 bool in_request) {
  TargetVerdict out;
  bool live = in_request;
  for (const auto& [role, relation] : relations) {
    if (role == 2 || role == pg::kNativeCommentRelationRole) continue;
    // The rows carrying the target, each with its versions.
    std::map<Uuid, std::vector<std::pair<const pg::RowDataRecord*, const catalog::CatalogMetadataVersion*>>> rows;
    for (const auto& leaf : relation.leaves)
      for (const auto& row : leaf.page.body.rows) {
        const auto m = leaf.metadata.find(row.version_uuid);
        if (m == leaf.metadata.end()) {
          out.error = E::invalid_catalog;
          return out;
        }
        const auto& h = m->second.record.header;
        if (h.object_uuid.value != target || h.kind == catalog::CatalogRecordKind::localized_name ||
            h.kind == catalog::CatalogRecordKind::localized_comment)
          continue;
        rows[row.row_uuid.value].emplace_back(&row, &m->second);
      }
    for (auto& [id, versions] : rows) {
      std::sort(versions.begin(), versions.end(),
                [](const auto& a, const auto& b) { return a.first->row_version > b.first->row_version; });
      for (const auto& [row, metadata] : versions) {
        const auto* creator = entry(row->local_transaction_id);
        if (!creator || creator->identity.transaction_uuid.value != row->transaction_uuid.value) {
          out.error = E::invalid_catalog;
          return out;
        }
        const auto state = mga::InventoryVisibilityState(*creator);
        if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal) continue;
        const bool dropped = metadata->record.header.deleted;
        if (row->local_transaction_id == order) {  // this transaction's own version decides
          live = live || !dropped;
          break;
        }
        if (InProgress(state)) {
          // Q87 = 1: only another transaction's drop conflicts; its ALTER (or
          // uncommitted create) is another row version: the one below decides.
          if (dropped) {
            if (!out.blocker) out.blocker = row->local_transaction_id;
            break;
          }
          continue;
        }
        live = live || !dropped;  // committed
        break;
      }
    }
  }
  if (out.blocker) out.error = E::write_conflict;
  else if (!live) out.error = E::missing_target;
  return out;
}

// ---- SB_CATALOG_DEPENDENT_CASCADE (owner 2026-11-13, Q86, Q87 = 1) ---------
// The classes of rows fully dependent on a definition, dropped with it in the
// same write. Comments are the only class now; triggers, indexes and
// constraints of a table are to be added here, each with its collector.
// SB_NATIVE_NAME_DROP_CASCADE (owner Q249, 2026-11-16: "dropping an object
// automatically drops all its name rows"): the object's name vector and
// entries are the second class, after its comments.
enum class DependentClass : std::uint8_t { comment, name };
inline constexpr std::array<DependentClass, 2> kDependentClasses{DependentClass::comment, DependentClass::name};

// One class's dependent rows of the dropped definition, as one relation's
// successors: its relation (read into the request's relation map), the drop
// versions, and the class's own tree (the comment tree).
struct Dependents {
  DependentClass kind = DependentClass::comment;
  u16 role = 0;
  pg::NativeCatalogRootReference head;
  std::vector<Succ> succs;
  std::optional<NameTree> tree;
  std::vector<catalog::CatalogCommentRecord> comments;  // per succ, owned (the records the stamps key)
  // SB_NATIVE_NAME_DROP_CASCADE: the name trees (kinds 22, 23) the name drops
  // stamp, and the base payloads the succs point to (stable addresses).
  std::vector<NameTree> name_trees;
  std::deque<catalog::CatalogNamePayload> base_names;
};

// The drop version of a dependent row: its base's metadata with this
// transaction as creator, the next definition version, the larger generation,
// lifecycle dropped, status retired, retired by this transaction; the payload
// unchanged. Checked as any successor's.
Stop BuildDependentDrop(const Relation& relation, const Uuid& row_id, Version base, u64 newest_row_version,
                        const Uuid& transaction, u64 order, u64 generation, const pg::NativeCatalogRoot& root, Succ& c,
                        bool named = false) {
  const auto& base_row = relation.leaves[base.leaf].page.body.rows[base.row];
  c.row = row_id;
  c.base = base;
  c.base_metadata = &relation.leaves[base.leaf].metadata.at(base_row.version_uuid);
  c.metadata = *c.base_metadata;
  c.metadata.creator_transaction_uuid = {UuidKind::transaction, transaction};
  c.metadata.creator_local_transaction_id = order;
  c.metadata.definition_version = c.base_metadata->definition_version + 1;
  c.metadata.catalog_generation = std::max(c.base_metadata->catalog_generation, generation);
  c.metadata.record.header.deleted = true;
  c.metadata.lifecycle = catalog::CatalogObjectLifecycle::dropped;
  c.metadata.status = catalog::CatalogObjectStatus::retired;
  c.metadata.retired_transaction_uuid = {UuidKind::transaction, transaction};
  // A name row's metadata is complete only once its envelope is encoded at
  // placement (PlacedImage checks FollowsAs there), as a request's name
  // successor's (SB_NATIVE_NAME_DROP_CASCADE).
  if (named) c.metadata.record.payload.clear();
  if (c.base_metadata->definition_version == std::numeric_limits<u64>::max() ||
      newest_row_version == std::numeric_limits<u64>::max() ||
      (!named && !FollowsAs(*c.base_metadata, c.metadata, row_id, transaction, order)))
    return Refuse(E::invalid_successor);
  if (AheadOfRoot(c.metadata, root)) return Refuse(E::ahead_of_catalog_root);
  c.added.row_uuid = {UuidKind::row, row_id};
  c.added.previous_version_uuid = base_row.version_uuid;
  c.added.transaction_uuid = {UuidKind::transaction, transaction};
  c.added.local_transaction_id = order;
  c.added.row_version = newest_row_version + 1;
  c.added.previous_row_version = base_row.row_version;
  // SB_CATALOG_OWN_VERSION_REPLACE (Q88 = 1): this transaction's own comment
  // is replaced in place by its drop.
  if (ReplacesOwn(*c.base_metadata, base_row, order)) MakeReplacement(c, base_row);
  return std::nullopt;
}

// The comments on `target` (class comment): every comment row the comment
// tree's target prefix holds (every row of role 7 without the tree; nothing
// without a role-7 relation). Any version by another transaction in progress
// is a write_conflict with it; a row whose newest version (rolled-back and dead
// ones passed over) is committed and not a drop gets a drop version; one that
// is this transaction's own and not a drop is replaced in place by its drop
// (owner Q88 = 1). Identities are assigned by the caller (not to
// replacements).
Stop CollectCommentDependents(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                              const pg::NativeCatalogRoot& root, std::vector<byte>& image, const Uuid& target,
                              const Uuid& transaction, u64 order, u64 generation,
                              const std::function<const mga::TransactionInventoryEntry*(u64)>& entry,
                              std::map<u16, Relation>& relations, Dependents& out) {
  out.kind = DependentClass::comment;
  out.role = pg::kNativeCommentRelationRole;
  const auto head = std::find_if(root.roots.begin(), root.roots.end(), [](const auto& r) { return r.role == pg::kNativeCommentRelationRole; });
  if (head == root.roots.end()) return std::nullopt;  // no comments relation: nothing depends here
  if (head->page_type != 6 && head->page_type != 0x200) return Refuse(E::invalid_catalog);
  out.head = *head;
  CatalogVersionWriteResult result;
  if (!ReadRelation(sink, binding, page_size, *head, relations[out.role], result)) return result;
  if (!ReadCommentTree(sink, binding, page_size, image, out.tree, result)) return result;
  const auto& relation = relations.at(out.role);
  std::set<Uuid> candidates;
  if (out.tree) {
    if (!ProbeTreePrefix(out.tree->tree, std::span<const byte>(target.bytes.data(), target.bytes.size()), candidates))
      return Refuse(E::invalid_catalog);
  } else {
    for (const auto& leaf : relation.leaves)
      for (const auto& row : leaf.page.body.rows) candidates.insert(row.row_uuid.value);
  }
  for (const auto& id : candidates) {  // row-UUID order
    std::optional<Version> newest;
    u64 newest_row_version = 0;
    bool any = false;
    for (std::size_t l = 0; l < relation.leaves.size(); ++l)
      for (std::size_t r = 0; r < relation.leaves[l].page.body.rows.size(); ++r) {
        const auto& row = relation.leaves[l].page.body.rows[r];
        if (row.row_uuid.value != id) continue;
        any = true;
        newest_row_version = std::max(newest_row_version, row.row_version);
        const auto* creator = entry(row.local_transaction_id);
        if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value ||
            !relation.leaves[l].metadata.contains(row.version_uuid))
          return Refuse(E::invalid_catalog);
        const auto state = mga::InventoryVisibilityState(*creator);
        if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal) continue;
        // Another transaction's insert, edit or drop of a comment on the
        // target, in progress: the drop and it write one row's dependents.
        if (InProgress(state) && row.local_transaction_id != order) return Conflict(row.local_transaction_id);
        if (!newest || row.row_version > relation.leaves[newest->leaf].page.body.rows[newest->row].row_version)
          newest = Version{l, r};
      }
    if (!any) return Refuse(E::invalid_catalog);  // a tree cell naming no row
    if (!newest) continue;                         // every version rolled back or dead
    const auto& base_row = relation.leaves[newest->leaf].page.body.rows[newest->row];
    const auto& base_metadata = relation.leaves[newest->leaf].metadata.at(base_row.version_uuid);
    if (base_metadata.record.header.kind != catalog::CatalogRecordKind::localized_comment) return Refuse(E::invalid_catalog);
    const auto decoded = catalog::DecodeCatalogComment(base_metadata.record.payload);
    if (!decoded.ok()) return Refuse(E::invalid_catalog);
    if (decoded.record->target_object_uuid != target) {
      if (out.tree) return Refuse(E::invalid_catalog);  // a cell under the target's prefix on another target's row
      continue;
    }
    if (base_metadata.record.header.deleted) continue;  // already dropped
    // (Superseded by owner Q88 = 1, 2026-11-13: an own comment is now
    // replaced in place by its drop, SB_CATALOG_OWN_VERSION_REPLACE. The
    // earlier note read: "OPEN (step4-open2, owner ruling pending): this
    // transaction's own live comment version cannot be superseded by it
    // here. Its tree cell would be stamped by its own creator, which the
    // B-tree cell codec refuses (superseding > creator) ... Refused before any
    // write, invalid_successor".)
    Succ c;
    if (auto stop = BuildDependentDrop(relation, id, *newest, newest_row_version, transaction, order, generation, root, c))
      return stop;
    out.succs.push_back(std::move(c));
    out.comments.push_back(catalog::MaterializeCatalogComment(*decoded.record));
  }
  return std::nullopt;
}

// SEARCH_KEY: SB_NATIVE_NAME_DROP_CASCADE (owner Q249, 2026-11-16). The
// dropped object's names (class name): its name vector (the role-2 row of
// subtype name_vector whose parent is the dropped object - found by the
// object's UUID, Q261(b) = 1, 2026-11-18; was: the row whose object is the
// definition's name_vector_uuid) and every entry of that vector (rows whose
// parent is the vector), in the names relation (role 2), row-UUID order. As
// for comments: any version by another transaction in progress is a
// write_conflict with it; a row whose newest version (rolled-back and dead
// ones passed over) is committed and live gets a drop version (a vector:
// lifecycle dropped; an entry: lifecycle dropped, dropped by this
// transaction); this transaction's own live version is replaced in place
// (owner Q88 = 1). The base versions' cells in both name trees are stamped
// with this transaction; a drop adds no cell. (Was: a definition drop left
// its names to the caller, which dropped the entry and the vector itself.)
Stop CollectNameDependents(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                           const pg::NativeCatalogRoot& root, std::vector<byte>& image, const Uuid& object_id,
                           const Uuid& transaction, u64 order, u64 generation,
                           const std::function<const mga::TransactionInventoryEntry*(u64)>& entry,
                           std::map<u16, Relation>& relations, Dependents& out) {
  out.kind = DependentClass::name;
  out.role = 2;
  if (object_id.is_nil()) return Refuse(E::invalid_successor);
  const auto head = std::find_if(root.roots.begin(), root.roots.end(), [](const auto& r) { return r.role == 2; });
  if (head == root.roots.end()) return std::nullopt;
  if (head->page_type != 6 && head->page_type != 0x200) return Refuse(E::invalid_catalog);
  out.head = *head;
  CatalogVersionWriteResult result;
  if (!relations.contains(2) && !ReadRelation(sink, binding, page_size, *head, relations[2], result)) return result;
  const auto& relation = relations.at(2);
  // Q261(b) = 1: the object's vector(s) first - the name_vector rows whose
  // parent is the object (every version of that row carries the same object
  // identity) - then the vector's entries. An object with no vector row is
  // unnamed: nothing cascades.
  std::set<Uuid> vector_ids;
  for (const auto& leaf : relation.leaves)
    for (const auto& row : leaf.page.body.rows) {
      const auto m = leaf.metadata.find(row.version_uuid);
      if (m == leaf.metadata.end()) return Refuse(E::invalid_catalog);
      const auto& h = m->second.record.header;
      if (h.parent_uuid.value == object_id && m->second.object_subtype == catalog::CatalogObjectSubtype::name_vector)
        vector_ids.insert(h.object_uuid.value);
    }
  if (vector_ids.empty()) return std::nullopt;  // an unnamed definition
  if (!ReadNameTrees(sink, binding, page_size, image, out.name_trees, result)) return result;
  std::map<Uuid, std::vector<Version>> rows;
  for (std::size_t l = 0; l < relation.leaves.size(); ++l)
    for (std::size_t r = 0; r < relation.leaves[l].page.body.rows.size(); ++r) {
      const auto& row = relation.leaves[l].page.body.rows[r];
      const auto m = relation.leaves[l].metadata.find(row.version_uuid);
      if (m == relation.leaves[l].metadata.end()) return Refuse(E::invalid_catalog);
      const auto& h = m->second.record.header;
      if (vector_ids.contains(h.object_uuid.value) || vector_ids.contains(h.parent_uuid.value)) rows[row.row_uuid.value].push_back({l, r});
    }
  for (const auto& [id, versions] : rows) {  // row-UUID order
    std::optional<Version> newest;
    u64 newest_row_version = 0;
    for (const auto& v : versions) {
      const auto& row = relation.leaves[v.leaf].page.body.rows[v.row];
      newest_row_version = std::max(newest_row_version, row.row_version);
      const auto* creator = entry(row.local_transaction_id);
      if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value) return Refuse(E::invalid_catalog);
      const auto state = mga::InventoryVisibilityState(*creator);
      if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal) continue;
      if (InProgress(state) && row.local_transaction_id != order) return Conflict(row.local_transaction_id);
      if (!newest || row.row_version > relation.leaves[newest->leaf].page.body.rows[newest->row].row_version) newest = v;
    }
    if (!newest) continue;
    const auto& leaf = relation.leaves[newest->leaf];
    const auto& base_row = leaf.page.body.rows[newest->row];
    const auto& base_metadata = leaf.metadata.at(base_row.version_uuid);
    if (base_metadata.record.header.deleted) continue;  // already dropped
    const catalog::CatalogNameVersionBinding at{
        {UuidKind::database, leaf.page.header.database_uuid}, {UuidKind::filespace, leaf.page.header.filespace_uuid},
        base_row.row_uuid, {UuidKind::row, base_row.version_uuid}, base_metadata.record.header.object_uuid,
        base_row.transaction_uuid, leaf.page.body.page_number, base_row.stable_slot_id, base_row.storage_generation,
        base_row.row_version, base_row.local_transaction_id, base_metadata.catalog_generation};
    const auto& bytes = base_metadata.record.payload;
    const auto decoded = catalog::DecodeCatalogNameEnvelopeView({reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, at);
    if (!decoded.ok()) return Refuse(E::invalid_catalog);
    out.base_names.push_back(catalog::MaterializeCatalogNameEnvelope(*decoded.record).payload);
    Succ c;
    if (auto stop = BuildDependentDrop(relation, id, *newest, newest_row_version, transaction, order, generation, root, c, true))
      return stop;
    auto dropped = out.base_names.back();
    std::visit([&](auto& r) { r.lifecycle_state = catalog::CatalogNameLifecycle::dropped; }, dropped);
    if (auto* e = std::get_if<catalog::CatalogNameEntry>(&dropped)) e->dropped_transaction_uuid = {UuidKind::transaction, transaction};
    c.name = std::move(dropped);
    c.base_name = &out.base_names.back();
    out.succs.push_back(std::move(c));
  }
  return std::nullopt;
}

// The hook: every dependent class's rows of the dropped definition `target`,
// then their identities from `identities` in class order (row-UUID order
// within a class). Too few: dependent_versions_exhausted with the count.
Stop CollectDependents(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding, u32 page_size,
                       const pg::NativeCatalogRoot& root, std::vector<byte>& image, const CatalogVersionRequest& request,
                       const std::function<const mga::TransactionInventoryEntry*(u64)>& entry,
                       std::map<u16, Relation>& relations, std::vector<Dependents>& out) {
  const auto& target = request.metadata.record.header.object_uuid.value;
  for (const auto kind : kDependentClasses) {
    Dependents d;
    switch (kind) {
      case DependentClass::comment:
        if (auto stop = CollectCommentDependents(sink, binding, page_size, root, image, target, request.transaction_uuid,
                                                 request.transaction_order, request.metadata.catalog_generation, entry,
                                                 relations, d))
          return stop;
        break;
      case DependentClass::name:
        if (auto stop = CollectNameDependents(sink, binding, page_size, root, image,
                                              target, request.transaction_uuid,  // Q261(b) = 1: by the dropped object
                                              request.transaction_order, request.metadata.catalog_generation, entry,
                                              relations, d))
          return stop;
        break;
    }
    if (!d.succs.empty()) out.push_back(std::move(d));
  }
  std::size_t needed = 0;
  for (const auto& d : out)
    for (const auto& c : d.succs) needed += c.replace ? 0 : 1;
  if (request.dependent_versions.size() < needed) {
    CatalogVersionWriteResult result;
    result.error = E::dependent_versions_exhausted;
    result.cascade_needed = needed;
    return result;
  }
  std::size_t used = 0;
  for (auto& d : out) {
    const auto& relation = relations.at(d.role);
    for (auto& c : d.succs) {
      if (c.replace) continue;  // SB_CATALOG_OWN_VERSION_REPLACE: keeps its version UUID
      const auto& id = request.dependent_versions[used];
      if (!V7(id) || id == request.version_uuid || id == request.row_uuid || id == request.predecessor_version_uuid)
        return Refuse(E::invalid_request);
      for (std::size_t j = 0; j < used; ++j)
        if (request.dependent_versions[j] == id) return Refuse(E::invalid_request);
      for (const auto& leaf : relation.leaves)
        for (const auto& row : leaf.page.body.rows)
          if (row.version_uuid == id || row.row_uuid.value == id) return Refuse(E::invalid_successor);
      c.version = id;
      c.added.version_uuid = id;
      ++used;
    }
  }
  return std::nullopt;
}
}  // namespace
namespace {
CatalogVersionWriteResult SuccessorPass(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                        const CatalogVersionRequest& request, PageSource& pages, bool& deferred) noexcept {
  CatalogVersionWriteResult result;
  try {
    // ---- the request alone -------------------------------------------------
    const auto& croot = binding.catalog_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(croot.page_size_profile_uuid);
    if (!V7(binding.database_uuid) || (croot.kind != 2 && croot.kind != 8) || !croot.page_number ||
        !croot.page_generation || !profile || binding.state_root.kind != 4 ||
        binding.state_root.page_size_profile_uuid != croot.page_size_profile_uuid ||
        binding.state_root.filespace_uuid != croot.filespace_uuid || !binding.maximum_chain_pages ||
        !binding.maximum_index_pages || !binding.maximum_base_pages || request.role < 1 || request.role > pg::kNativeCatalogRelationRoles ||
        !request.transaction_order || !V7(request.transaction_uuid) || !V7(request.row_uuid) ||
        !V7(request.predecessor_version_uuid) || !V7(request.version_uuid) ||
        request.version_uuid == request.predecessor_version_uuid || request.version_uuid == request.row_uuid)
      return result;
    if (binding.read_only_database) return Refuse(E::read_only_database);
    // SB_NATIVE_NAME_TREES (N4a): a name row carries its name and an empty
    // payload (the envelope is encoded once placed), and agrees with it.
    const bool named = request.metadata.record.header.kind == catalog::CatalogRecordKind::localized_name;
    if (named && request.name.has_value() && !request.metadata.record.header.deleted)
      if (const auto text = NameTextRefusal(*request.name)) return Refuse(*text);  // S6: the specific text code first
    if (named != request.name.has_value() || (named && !request.metadata.record.payload.empty()) ||
        (named && !catalog::CatalogNamePayloadMatchesMetadata(*request.name, request.metadata)))
      return Refuse(E::invalid_successor);
    const u32 page_size = profile->page_size_bytes;

    // ---- the transaction ---------------------------------------------------
    pg::TransactionWriterBinding tx;
    tx.database_uuid = binding.database_uuid;
    tx.state_root = binding.state_root;
    tx.maximum_chain_pages = binding.maximum_chain_pages;
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tx);
    if (!chains.ok()) {
      result.error = chains.error == pg::TransactionWriterError::io_failure ? E::io_failure
                     : chains.error == pg::TransactionWriterError::resource_exhausted ? E::resource_exhausted
                                                                                       : E::invalid_inventory;
      result.inventory_error = chains.page_error;
      result.io = chains.io;
      return result;
    }
    const auto built = pg::BuildLocalTransactionInventoryFromImages(page_size, chains.state_images, chains.header_images);
    if (!built.ok()) {
      result.error = E::invalid_inventory;
      result.inventory_error = built.error;
      return result;
    }
    const auto& entries = built.inventory.entries;
    const auto entry = [&](u64 order) -> const mga::TransactionInventoryEntry* {
      const auto at = std::lower_bound(entries.begin(), entries.end(), order,
                                       [](const auto& e, u64 n) { return e.identity.local_id.value < n; });
      return at == entries.end() || at->identity.local_id.value != order ? nullptr : &*at;
    };
    const auto* self = entry(request.transaction_order);
    if (!self || self->identity.transaction_uuid.value != request.transaction_uuid)
      return Refuse(E::transaction_not_active);
    if (self->state == mga::TransactionState::read_only_active) return Refuse(E::read_only_transaction);
    if (self->state != mga::TransactionState::active) return Refuse(E::transaction_not_active);

    // ---- the catalog root and the relation head ----------------------------
    std::vector<byte> image;
    if (!ReadPage(sink, page_size, croot.page_number, image, result)) return result;
    const disk::NativePageReference root_ref{croot.filespace_uuid, croot.page_number, croot.page_generation,
                                             croot.page_size_profile_uuid};
    if (!HeaderBinds(image, binding.database_uuid, root_ref, croot.page_type)) return Refuse(E::invalid_catalog);
    const auto root = pg::DecodeNativeCatalogRoot(image);
    if (!root.ok() || root.root->object_uuid != croot.object_uuid) return Refuse(E::invalid_catalog);
    const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                   [&](const auto& r) { return r.role == request.role; });
    if (head == root.root->roots.end() || (head->page_type != 6 && head->page_type != 0x200))
      return Refuse(E::invalid_catalog);

    // ---- every base leaf the head reaches ----------------------------------
    std::map<u16, Relation> relations;
    if (!ReadRelation(sink, binding, page_size, *head, relations[request.role], result)) return result;
    if (named && request.role != 2) return Refuse(E::invalid_role);
    // SB_NATIVE_COMMENT_INSERT: a comment row's successor is in role 7, and
    // role 7 holds nothing else.
    const bool commented = request.metadata.record.header.kind == catalog::CatalogRecordKind::localized_comment;
    if (commented != (request.role == pg::kNativeCommentRelationRole)) return Refuse(E::invalid_role);
    if (named) {
      // The names relation (this one) and the profiles and policies an
      // entry names.
      const auto profiles = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                         [&](const auto& r) { return r.role == 5; });
      if (profiles == root.root->roots.end() || (profiles->page_type != 6 && profiles->page_type != 0x200))
        return Refuse(E::invalid_catalog);
      if (!ReadRelation(sink, binding, page_size, *profiles, relations[5], result)) return result;
    }
    // SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): an event is immutable; a
    // definition version cites an event of this transaction.
    if (request.metadata.record.header.kind == catalog::CatalogRecordKind::audit_evidence)
      return Refuse(E::invalid_successor);
    // Comment rows are data (owner Q79 = 1): no event either.
    if (!named && !commented) {
      if (!relations.contains(6)) {
        const auto events = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                         [&](const auto& r) { return r.role == 6; });
        if (events == root.root->roots.end() || (events->page_type != 6 && events->page_type != 0x200))
          return Refuse(E::invalid_catalog);
        if (!ReadRelation(sink, binding, page_size, *events, relations[6], result)) return result;
      }
      if (!StoredOwnEvent(relations.at(6), request.metadata.audit_uuid, request.transaction_uuid, request.transaction_order))
        return Refuse(E::unaudited_change);
    }
    auto& relation_read = relations.at(request.role);
    auto& bases = relation_read.bases;
    auto& leaves = relation_read.leaves;

    // ---- the row's versions, newest first ----------------------------------
    // SB_CATALOG_SINGLE_WRITER: the conflict decided from these images stays
    // decided only while no other catalog writer runs before the write.
    std::vector<Version> versions;
    for (std::size_t l = 0; l < leaves.size(); ++l)
      for (std::size_t r = 0; r < leaves[l].page.body.rows.size(); ++r)
        if (leaves[l].page.body.rows[r].row_uuid.value == request.row_uuid) versions.push_back({l, r});
    if (versions.empty()) return Refuse(E::row_not_found);
    const auto row_of = [&](const Version& v) -> pg::RowDataRecord& { return leaves[v.leaf].page.body.rows[v.row]; };
    std::sort(versions.begin(), versions.end(),
              [&](const auto& a, const auto& b) { return row_of(a).row_version > row_of(b).row_version; });
    const u64 newest = row_of(versions.front()).row_version;
    if (newest == std::numeric_limits<u64>::max()) return Refuse(E::invalid_successor);
    std::optional<Version> base;
    for (const auto& v : versions) {
      const auto& row = row_of(v);
      const auto* creator = entry(row.local_transaction_id);
      if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value)
        return Refuse(E::invalid_catalog);
      if (row.local_transaction_id == request.transaction_order) {
        base = v;
        break;
      }
      const auto state = mga::InventoryVisibilityState(*creator);
      if (InProgress(state)) {
        result.error = E::write_conflict;
        result.blocker_order = row.local_transaction_id;
        return result;
      }
      if (state == mga::TransactionState::committed) {
        base = v;
        break;
      }
    }
    if (!base) return Refuse(E::row_not_found);
    auto& base_row = row_of(*base);
    result.base_version_uuid = base_row.version_uuid;
    if (base_row.version_uuid != request.predecessor_version_uuid) {
      result.error = E::update_conflict;
      return result;
    }
    const auto& base_metadata = leaves[base->leaf].metadata.at(base_row.version_uuid);
    if (base_metadata.record.header.deleted) {
      result.error = E::row_deleted;
      return result;
    }
    // SB_NATIVE_NAME_DEFAULT_SWITCH (owner Q228 revisited): the paired entry
    // row of a default switch, found and conflict-checked as the request's.
    std::optional<Version> pair_base;
    u64 pair_newest = 0;
    if (request.paired) {
      const auto& pr = *request.paired;
      if (!named || request.role != 2 || !V7(pr.row_uuid) || !V7(pr.predecessor_version_uuid) || !V7(pr.version_uuid) ||
          pr.row_uuid == request.row_uuid || pr.version_uuid == request.version_uuid ||
          pr.version_uuid == pr.predecessor_version_uuid || pr.version_uuid == request.row_uuid ||
          pr.version_uuid == pr.row_uuid || request.metadata.record.header.deleted || pr.metadata.record.header.deleted ||
          pr.metadata.record.header.kind != catalog::CatalogRecordKind::localized_name ||
          !pr.metadata.record.payload.empty() || !catalog::CatalogNamePayloadMatchesMetadata(pr.name, pr.metadata) ||
          !std::holds_alternative<catalog::CatalogNameEntry>(pr.name) ||
          !std::holds_alternative<catalog::CatalogNameEntry>(*request.name))
        return Refuse(E::invalid_successor);
      std::vector<Version> pair_versions;
      for (std::size_t l = 0; l < leaves.size(); ++l)
        for (std::size_t r = 0; r < leaves[l].page.body.rows.size(); ++r)
          if (leaves[l].page.body.rows[r].row_uuid.value == pr.row_uuid) pair_versions.push_back({l, r});
      if (pair_versions.empty()) return Refuse(E::row_not_found);
      std::sort(pair_versions.begin(), pair_versions.end(),
                [&](const auto& a, const auto& b) { return row_of(a).row_version > row_of(b).row_version; });
      pair_newest = row_of(pair_versions.front()).row_version;
      if (pair_newest == std::numeric_limits<u64>::max()) return Refuse(E::invalid_successor);
      for (const auto& v : pair_versions) {
        const auto& row = row_of(v);
        const auto* creator = entry(row.local_transaction_id);
        if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value)
          return Refuse(E::invalid_catalog);
        if (row.local_transaction_id == request.transaction_order) {
          pair_base = v;
          break;
        }
        const auto state = mga::InventoryVisibilityState(*creator);
        if (InProgress(state)) {
          result.error = E::write_conflict;
          result.blocker_order = row.local_transaction_id;
          return result;
        }
        if (state == mga::TransactionState::committed) {
          pair_base = v;
          break;
        }
      }
      if (!pair_base) return Refuse(E::row_not_found);
      if (row_of(*pair_base).version_uuid != pr.predecessor_version_uuid) {
        result.error = E::update_conflict;
        result.base_version_uuid = row_of(*pair_base).version_uuid;
        return result;
      }
      if (leaves[pair_base->leaf].metadata.at(row_of(*pair_base).version_uuid).record.header.deleted) {
        result.error = E::row_deleted;
        return result;
      }
    }
    // A name row's metadata is complete only once its envelope is encoded at
    // placement; Follows is checked there. Everything else is checked now.
    if (!named && !Follows(base_metadata, request.metadata, request)) {
      result.error = E::invalid_successor;
      return result;
    }
    // SB_NATIVE_COMMENT_INSERT (owner Q82 = 1): an edit or a drop keeps the
    // comment UUID, the target and the language; only the text changes.
    std::optional<catalog::CatalogCommentRecordView> comment_record;
    std::optional<NameTree> comment_tree;
    if (commented) {
      const auto before = catalog::DecodeCatalogComment(base_metadata.record.payload);
      const auto after = catalog::DecodeCatalogComment(request.metadata.record.payload);
      if (!before.ok()) return Refuse(E::invalid_catalog);
      if (!after.ok() || !catalog::CatalogCommentPreservesIdentity(*before.record, *after.record))
        return Refuse(E::invalid_successor);
      comment_record = *after.record;
      // SB_NATIVE_COMMENT_TARGET (owner 2026-11-13, Q86, Q87 = 1): an edit
      // names a target that is a live definition in this transaction's view
      // (a drop of the comment does not look).
      if (!request.metadata.record.header.deleted) {
        if (!ReadDefinitionRelations(sink, binding, page_size, *root.root, relations, nullptr, result)) return result;
        const auto verdict = CheckCommentTarget(relations, comment_record->target_object_uuid, entry,
                                                request.transaction_order, false);
        if (verdict.error != E::none) {
          result.error = verdict.error;
          result.blocker_order = verdict.blocker;
          return result;
        }
      }
      // SB_NATIVE_COMMENT_TREE: read now; maintained with the other trees.
      if (!ReadCommentTree(sink, binding, page_size, image, comment_tree, result)) return result;
    }
    // SB_NATIVE_NAME_TREES (N4a): the name's rules.
    std::optional<NameState> names;
    const catalog::CatalogNamePayload* base_name = nullptr;
    // The successor (SB_NATIVE_NAME_ROW_V4: a name change writes only its own
    // row; was: then each entry whose stored path the change moved, in
    // row-UUID order, SB_NATIVE_NAME_CASCADE).
    std::vector<Succ> succs;
    std::optional<catalog::CatalogNameIndex> moved_paths;
    std::optional<catalog::IdentifierFoldMode> own_fold, pair_fold;
    const catalog::CatalogNamePayload* pair_base_name = nullptr;  // SB_NATIVE_NAME_DEFAULT_SWITCH
    // SB_NATIVE_NAME_TREES (Q42): each base version's cells are stamped with
    // this transaction; a version that is not a drop gets its own cells.
    std::vector<NameTree> name_trees;
    if (named) {
      std::vector<const catalog::CatalogNamePayload*> request_names{&*request.name};
      if (request.paired) request_names.push_back(&request.paired->name);
      names.emplace(binding, request.transaction_order, relations, entry, std::move(request_names));
      if (!names->Load()) return Refuse(E::invalid_catalog);
      for (const auto& s : names->Stored()) {
        if (s.row == request.row_uuid && s.row_version == base_row.row_version) base_name = &s.payload;
        if (pair_base && s.row == request.paired->row_uuid && s.row_version == row_of(*pair_base).row_version)
          pair_base_name = &s.payload;
      }
      if (!base_name || !catalog::CatalogNamePayloadPreservesIdentity(*base_name, *request.name))
        return Refuse(E::invalid_successor);
      // SB_NATIVE_NAME_DEFAULT_SWITCH: the paired row is another entry of the
      // same object.
      if (request.paired &&
          (!pair_base_name || !catalog::CatalogNamePayloadPreservesIdentity(*pair_base_name, request.paired->name) ||
           std::get<catalog::CatalogNameEntry>(request.paired->name).object_uuid.value !=
               std::get<catalog::CatalogNameEntry>(*request.name).object_uuid.value))
        return Refuse(E::invalid_successor);
      // SB_NATIVE_NAME_TREES: read before the checks; the collision check
      // probes the authoritative tree (SB_NATIVE_NAME_PROBE, N3).
      if (!ReadNameTrees(sink, binding, page_size, image, name_trees, result)) return result;
      if (!name_trees.empty()) names->ProbeWith(&name_trees[0].tree);
      if (name_trees.size() > 1) names->DefaultIndexWith(&name_trees[1].tree);
      // Owner Q250, Q227, Q235 = 1, Q228 revisited, Q247 (2026-11-15/16): the
      // name list rules (CheckNameListChange), before any write.
      {
        const auto read_definitions = [&]() -> Stop {
          if (!ReadDefinitionRelations(sink, binding, page_size, *root.root, relations, nullptr, result)) return result;
          return std::nullopt;
        };
        std::optional<std::pair<Uuid, const catalog::CatalogNameEntry*>> to_pair, to_primary;
        if (request.paired) {
          to_pair.emplace(request.paired->row_uuid, &std::get<catalog::CatalogNameEntry>(request.paired->name));
          to_primary.emplace(request.row_uuid, &std::get<catalog::CatalogNameEntry>(*request.name));
        }
        if (auto stop = CheckNameListChange(*names, request.row_uuid, *base_name, *request.name,
                                            request.metadata.record.header.deleted, request.transaction_order, relations,
                                            read_definitions, to_pair ? &*to_pair : nullptr))
          return *stop;
        if (request.paired)
          if (auto stop = CheckNameListChange(*names, request.paired->row_uuid, *pair_base_name, request.paired->name, false,
                                              request.transaction_order, relations, read_definitions, &*to_primary))
            return *stop;
      }
      const auto* entry_payload = std::get_if<catalog::CatalogNameEntry>(&*request.name);
      if (!request.metadata.record.header.deleted && entry_payload) {
        const auto verdict = names->CheckEntry(*request.name);
        if (verdict.error == E::invalid_catalog) return Refuse(E::invalid_catalog);
        if (verdict.error != E::none) {
          // Owner Q226: a name-key uniqueness write_conflict names its blocker.
          // (Was: Refuse(verdict.error), a fresh result, so a successor's
          // write_conflict from CheckEntry carried no blocker_order.)
          auto refused = Refuse(verdict.error);
          refused.blocker_order = verdict.blocker;
          return refused;
        }
        own_fold = verdict.own_fold;
        if (!own_fold) return Refuse(E::invalid_name);
      }
      if (request.paired) {  // SB_NATIVE_NAME_DEFAULT_SWITCH: the paired entry's keys and collisions
        const auto verdict = names->CheckEntry(request.paired->name);
        if (verdict.error == E::invalid_catalog) return Refuse(E::invalid_catalog);
        if (verdict.error != E::none) {
          auto refused = Refuse(verdict.error);
          refused.blocker_order = verdict.blocker;
          return refused;
        }
        pair_fold = verdict.own_fold;
        if (!pair_fold) return Refuse(E::invalid_name);
      }
      // The names a path runs through after the change: the stored names this
      // transaction sees, with the request's row replaced by its new version
      // (removed when it is a drop). Owner Q226: checked in this
      // transaction's view only. (Was, Q212: CheckPathsAfterChange also built,
      // per other transaction with an entry in progress, its own views. Was:
      // also before_paths, the names before the change as this transaction
      // sees them.)
      const auto* replacement = request.metadata.record.header.deleted ? nullptr : &*request.name;
      // SB_NATIVE_NAME_DEFAULT_SWITCH: both rows' new versions at once.
      std::vector<const catalog::CatalogNamePayload*> added_names;
      if (request.paired) added_names.push_back(&request.paired->name);
      moved_paths = names->StoredPaths(request.row_uuid, replacement, added_names);
      const std::array<Uuid, 2> skip{request.row_uuid, request.paired ? request.paired->row_uuid : request.row_uuid};
      if (auto stop = CheckPathsAfterChange(*names, *moved_paths, skip, E::invalid_successor)) return *stop;
    }
    {
      // A runtime authority binding's payload catalog_generation and
      // security_epoch equal its metadata's (that family's own rule, checked
      // above), so this also keeps every binding within the root.
      if (AheadOfRoot(request.metadata, *root.root)) return Refuse(E::ahead_of_catalog_root);
    }
    for (const auto& leaf : leaves)
      for (const auto& row : leaf.page.body.rows) {
        if (row.version_uuid == request.version_uuid) return Refuse(E::invalid_successor);
      }
    // SB_CATALOG_DEPENDENT_CASCADE (owner 2026-11-13, Q86, Q87 = 1): a drop
    // of a definition (not a name row, not a comment row) takes its fully
    // dependent rows with it, in this write; every refusal here comes before
    // any write.
    std::vector<Dependents> dependents;
    if (request.metadata.record.header.deleted && !named && !commented)
      if (auto stop = CollectDependents(sink, binding, page_size, *root.root, image, request, entry, relations, dependents))
        return *stop;

    // ---- the new rows ------------------------------------------------------
    {
      Succ primary;
      primary.row = request.row_uuid;
      primary.version = request.version_uuid;
      primary.base = *base;
      primary.base_metadata = &base_metadata;
      primary.metadata = request.metadata;
      primary.name = request.name;
      primary.base_name = base_name;
      if (named && !request.metadata.record.header.deleted)
        if (const auto* e = std::get_if<catalog::CatalogNameEntry>(&*request.name)) {
          if (catalog::CheckCatalogNameEntryPath(catalog::BorrowCatalogNameEntry(*e), *moved_paths) !=
              catalog::CatalogNamePathError::none)
            return Refuse(E::invalid_name);
          primary.own_fold = own_fold;
        }
      auto& added = primary.added;
      added.row_uuid = {UuidKind::row, request.row_uuid};
      added.version_uuid = request.version_uuid;
      added.previous_version_uuid = base_row.version_uuid;
      added.transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
      added.local_transaction_id = request.transaction_order;
      added.row_version = newest + 1;
      added.previous_row_version = base_row.row_version;
      // SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1): a name or comment
      // row's successor of this transaction's own version replaces it in
      // place; request.version_uuid is then unused.
      if (ReplacesOwn(base_metadata, base_row, request.transaction_order)) MakeReplacement(primary, base_row);
      succs.insert(succs.begin(), std::move(primary));
    }
    // SB_NATIVE_NAME_DEFAULT_SWITCH: the paired entry's version, after it.
    if (request.paired) {
      const auto& pr = *request.paired;
      auto& pair_row = row_of(*pair_base);
      const auto& pair_metadata = leaves[pair_base->leaf].metadata.at(pair_row.version_uuid);
      for (const auto& leaf : leaves)
        for (const auto& row : leaf.page.body.rows)
          if (row.version_uuid == pr.version_uuid) return Refuse(E::invalid_successor);
      if (AheadOfRoot(pr.metadata, *root.root)) return Refuse(E::ahead_of_catalog_root);
      if (catalog::CheckCatalogNameEntryPath(catalog::BorrowCatalogNameEntry(std::get<catalog::CatalogNameEntry>(pr.name)),
                                             *moved_paths) != catalog::CatalogNamePathError::none)
        return Refuse(E::invalid_name);
      Succ paired;
      paired.row = pr.row_uuid;
      paired.version = pr.version_uuid;
      paired.base = *pair_base;
      paired.base_metadata = &pair_metadata;
      paired.metadata = pr.metadata;
      paired.name = pr.name;
      paired.base_name = pair_base_name;
      paired.own_fold = pair_fold;
      auto& added = paired.added;
      added.row_uuid = {UuidKind::row, pr.row_uuid};
      added.version_uuid = pr.version_uuid;
      added.previous_version_uuid = pair_row.version_uuid;
      added.transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
      added.local_transaction_id = request.transaction_order;
      added.row_version = pair_newest + 1;
      added.previous_row_version = pair_row.row_version;
      if (ReplacesOwn(pair_metadata, pair_row, request.transaction_order)) MakeReplacement(paired, pair_row);
      succs.push_back(std::move(paired));
    }
    // Placement (SB_NATIVE_NAME_CASCADE): PlaceSuccs; pages carrying forward
    // links are written last.
    std::vector<NativeCatalogLeafPage> work;
    work.reserve(leaves.size());
    for (const auto& leaf : leaves) work.push_back(leaf.page);
    std::set<std::size_t> link_pages;
    const std::size_t original_leaves = leaves.size();
    if (auto stop = PlaceSuccs(sink, binding, succs, relation_read, *head, work, link_pages, pages, page_size,
                               request.transaction_uuid, request.transaction_order))
      return *stop;
    std::set<std::size_t> changed_leaves = link_pages;
    for (const auto& c : succs) changed_leaves.insert(*c.target);
    std::map<std::size_t, NativeCatalogLeafResult> leaf_images;
    for (const auto i : changed_leaves) {
      auto encoded = EncodeNativeCatalogLeaf(work[i]);
      if (!encoded.ok()) {
        result.error = encoded.error == NativeCatalogLeafError::resource_exhausted ? E::resource_exhausted
                                                                                   : E::invalid_successor;
        result.leaf_error = encoded.error;
        return result;
      }
      leaf_images.emplace(i, std::move(encoded));
    }
    std::set<std::size_t> changed_index_pages;
    // SB_CATALOG_OWN_VERSION_MOVE (Q263 = 1): moved replacements' row-UUID cells first.
    if (auto stop = RetargetMovedRowCells(succs, relation_read, changed_index_pages)) return *stop;
    if (auto stop = IndexNewLeafVersions(sink, binding, succs, relation_read, original_leaves, changed_index_pages, pages,
                                         page_size, request.transaction_order))
      return *stop;
    // SB_CATALOG_DEPENDENT_CASCADE: each class's drop versions placed in its
    // relation as successors are (pages carrying forward links written after
    // every other leaf), new leaves indexed, the base versions' tree cells
    // stamped (a drop adds no cell).
    struct DependentPages {
      std::vector<NativeCatalogLeafPage> work;
      std::set<std::size_t> link_pages, changed_leaves, changed_index_pages;
      std::map<std::size_t, NativeCatalogLeafResult> images;
    };
    std::vector<DependentPages> dependent_pages(dependents.size());
    for (std::size_t d = 0; d < dependents.size(); ++d) {
      auto& dep = dependents[d];
      auto& at = dependent_pages[d];
      auto& relation = relations.at(dep.role);
      for (const auto& leaf : relation.leaves) at.work.push_back(leaf.page);
      const std::size_t before = relation.leaves.size();
      if (auto stop = PlaceSuccs(sink, binding, dep.succs, relation, dep.head, at.work, at.link_pages, pages, page_size,
                                 request.transaction_uuid, request.transaction_order))
        return *stop;
      at.changed_leaves = at.link_pages;
      for (const auto& c : dep.succs) at.changed_leaves.insert(*c.target);
      for (const auto i : at.changed_leaves) {
        auto encoded = EncodeNativeCatalogLeaf(at.work[i]);
        if (!encoded.ok()) {
          result.error = encoded.error == NativeCatalogLeafError::resource_exhausted ? E::resource_exhausted
                                                                                     : E::invalid_successor;
          result.leaf_error = encoded.error;
          return result;
        }
        at.images.emplace(i, std::move(encoded));
      }
      if (auto stop = RetargetMovedRowCells(dep.succs, relation, at.changed_index_pages)) return *stop;  // Q263 = 1
      if (auto stop = IndexNewLeafVersions(sink, binding, dep.succs, relation, before, at.changed_index_pages, pages,
                                           page_size, request.transaction_order))
        return *stop;
      switch (dep.kind) {
        case DependentClass::comment:
          if (dep.tree)
            for (const auto& c : dep.succs)
              if (auto stop = RemoveReplacedCommentCell(c, *dep.tree)) return *stop;
          if (dep.tree)
            for (std::size_t i = 0; i < dep.succs.size(); ++i) {
              const catalog::CatalogCommentRecordView view{dep.comments[i].comment_uuid,
                                                           dep.comments[i].target_object_uuid,
                                                           dep.comments[i].language_uuid, dep.comments[i].comment};
              if (auto stop = StampAndAddCommentCell(sink, binding, dep.succs[i], relation, *dep.tree, entry, pages,
                                                     page_size, request.transaction_order, view))
                return *stop;
            }
          break;
        case DependentClass::name:  // SB_NATIVE_NAME_DROP_CASCADE: both name trees, as a name successor's
          if (!dep.name_trees.empty()) {
            if (auto stop = RemoveReplacedNameCells(dep.succs, dep.name_trees)) return *stop;
            if (auto stop = StampAndAddNameCells(sink, binding, dep.succs, relation, dep.name_trees.data(), entry, pages,
                                                 page_size, request.transaction_order))
              return *stop;
          }
          break;
      }
    }

    // ---- the name trees (SB_NATIVE_NAME_TREES, owner Q42) ------------------
    // Each base version's cells are stamped with this transaction; a version
    // that is not a drop gets its own cells. Both trees or neither, from page
    // zero (read above, before the collision check that probes them); a
    // database without them has nothing to maintain.
    if (!name_trees.empty()) {
      if (auto stop = RemoveReplacedNameCells(succs, name_trees)) return *stop;
      if (auto stop = StampAndAddNameCells(sink, binding, succs, relation_read, name_trees.data(), entry, pages,
                                           page_size, request.transaction_order))
        return *stop;
    }
    // SB_NATIVE_COMMENT_TREE: the base version's cell is stamped with this
    // transaction; a version that is not a drop gets its own cell.
    if (comment_tree)
      if (auto stop = RemoveReplacedCommentCell(succs.front(), *comment_tree)) return *stop;
    if (comment_tree)
      if (auto stop = StampAndAddCommentCell(sink, binding, succs.front(), relation_read, *comment_tree, entry, pages,
                                             page_size, request.transaction_order, *comment_record))
        return *stop;
    // SB_CATALOG_OWN_VERSION_REPLACE: the replaced versions' cells, removed
    // and written first (their pages' images as read become those images).
    std::vector<RemovalWrite> removals;
    {
      std::vector<NameTree*> trees;
      for (auto& t : name_trees) trees.push_back(&t);
      if (comment_tree) trees.push_back(&*comment_tree);
      for (auto& d : dependents) {
        if (d.tree) trees.push_back(&*d.tree);
        for (auto& t : d.name_trees) trees.push_back(&t);  // SB_NATIVE_NAME_DROP_CASCADE
      }
      for (auto* t : trees)
        if (const auto error = BuildRemovalWrites(*t, removals)) return Refuse(*error);
    }

    if (pages.needed()) {
      deferred = true;
      result.error = E::none;
      return result;
    }
    // ---- the writes --------------------------------------------------------
    if (!ExecuteRemovals(sink, binding, page_size, removals, result)) return result;
    disk::CarefulWritePlan plan;
    // Every changed base leaf, the request's relation's and each dependent
    // class's (SB_CATALOG_DEPENDENT_CASCADE): those carrying a forward link
    // after every other leaf.
    struct LeafWrite {
      u64 page_number;
      const std::vector<byte>* image;
      const std::vector<byte>* as_read;
      bool linked;
    };
    std::vector<LeafWrite> leaf_writes;
    for (const auto i : changed_leaves)
      leaf_writes.push_back({bases[i].page_number, &leaf_images.at(i).bytes, &leaves[i].image, link_pages.contains(i)});
    for (std::size_t d = 0; d < dependents.size(); ++d) {
      const auto& relation = relations.at(dependents[d].role);
      for (const auto i : dependent_pages[d].changed_leaves)
        leaf_writes.push_back({relation.bases[i].page_number, &dependent_pages[d].images.at(i).bytes,
                               &relation.leaves[i].image, dependent_pages[d].link_pages.contains(i)});
    }
    std::vector<disk::CarefulWriteNodeId> unlinked_nodes, leaf_nodes;
    for (const bool linked : {false, true})
      for (const auto& w : leaf_writes) {
        if (w.linked != linked) continue;
        const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(*w.image),
                                   std::span<const byte>(*w.as_read));
        if (!node.ok()) return Refuse(E::plan_refused);
        if (linked)
          for (const auto before : unlinked_nodes)
            if (plan.Before(before, node.node) != CarefulWriteError::none) return Refuse(E::plan_refused);
        (linked ? leaf_nodes : unlinked_nodes).push_back(node.node);
      }
    leaf_nodes.insert(leaf_nodes.end(), unlinked_nodes.begin(), unlinked_nodes.end());
    // The row-UUID tree pages a new leaf's cell changed and the name tree
    // pages, after every base leaf; a page a split added before every
    // existing tree page (one of which points to it). Each image is kept
    // until the plan executes.
    struct TreeWrite {
      TreePage* page;
      std::vector<byte> image;
    };
    std::vector<TreeWrite> tree_writes;
    for (const auto t : changed_index_pages) tree_writes.push_back({&relation_read.tree[t], {}});
    for (auto& tree : name_trees)
      for (const auto t : tree.changed) tree_writes.push_back({&tree.tree[t], {}});
    if (comment_tree)
      for (const auto t : comment_tree->changed) tree_writes.push_back({&comment_tree->tree[t], {}});
    for (std::size_t d = 0; d < dependents.size(); ++d) {
      auto& relation = relations.at(dependents[d].role);
      for (const auto t : dependent_pages[d].changed_index_pages) tree_writes.push_back({&relation.tree[t], {}});
      if (dependents[d].tree)
        for (const auto t : dependents[d].tree->changed) tree_writes.push_back({&dependents[d].tree->tree[t], {}});
      for (auto& tree : dependents[d].name_trees)  // SB_NATIVE_NAME_DROP_CASCADE
        for (const auto t : tree.changed) tree_writes.push_back({&tree.tree[t], {}});
    }
    for (auto& w : tree_writes) {
      auto encoded = pg::EncodeNativeBtreePage(w.page->page);
      if (!encoded.ok()) return Refuse(E::invalid_catalog);
      w.image = std::move(encoded.bytes);
    }
    std::vector<disk::CarefulWriteNodeId> earlier = leaf_nodes;
    for (int rank = 0; rank <= 4 * static_cast<int>(NativeCatalogBtreeShape::kMaximumDepth) + 3; ++rank) {
      std::vector<disk::CarefulWriteNodeId> these;
      for (const auto& w : tree_writes) {
        if (w.page->rank != rank) continue;
        const auto node = plan.Add(w.page->ref.page_number * u64{page_size}, std::span<const byte>(w.image),
                                   std::span<const byte>(w.page->image));
        if (!node.ok()) return Refuse(E::plan_refused);
        for (const auto before : earlier)
          if (plan.Before(before, node.node) != CarefulWriteError::none) return Refuse(E::plan_refused);
        these.push_back(node.node);
      }
      earlier.insert(earlier.end(), these.begin(), these.end());
    }
    const auto r = plan.Execute(sink, binding.barrier_policy);
    result.write_error = r.error;
    result.io = r.io;
    result.writes += r.written;  // after the removal writes, if any
    result.syncs += r.syncs;
    result.page_number = bases[*succs.front().target].page_number;
    result.version_uuid = succs.front().version;
    result.replaced = succs.front().replace;
    result.forward_link_page = bases[base->leaf].page_number;
    result.row_version = succs.front().added.row_version;

    for (const auto& d : dependents)
      for (const auto& c : d.succs) result.dependents.emplace_back(c.row, c.version);
    if (r.ok()) {
      result.error = E::none;
      return result;
    }
    result.attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                       r.error == CarefulWriteError::barrier_sync_failed ||
                       r.error == CarefulWriteError::final_sync_failed || r.error == CarefulWriteError::sink_exception;
    result.error = result.attempted                                      ? E::write_failed
                   : r.error == CarefulWriteError::preimage_read_failed ? E::io_failure
                   : r.error == CarefulWriteError::preimage_mismatch    ? E::invalid_catalog
                                                                        : E::plan_refused;
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse(E::resource_exhausted);
  } catch (...) {
    return Refuse(E::invalid_request);
  }
}
}  // namespace

CatalogVersionWriteResult WriteCatalogSuccessorVersion(disk::CarefulWriteSink& sink,
                                                       const CatalogVersionWriterBinding& binding,
                                                       const CatalogVersionRequest& request) noexcept;

namespace {
CatalogVersionWriteResult InsertPass(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                     const CatalogInsertRequest& request, PageSource& pages, bool& deferred) noexcept {
  CatalogVersionWriteResult result;
  const auto refuse_row = [&](E error, std::size_t row) {
    result.error = error;
    result.failed_row = row;
    return result;
  };
  try {
    // ---- the request alone -------------------------------------------------
    const auto& croot = binding.catalog_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(croot.page_size_profile_uuid);
    if (!V7(binding.database_uuid) || (croot.kind != 2 && croot.kind != 8) || !croot.page_number ||
        !croot.page_generation || !profile || binding.state_root.kind != 4 ||
        binding.state_root.page_size_profile_uuid != croot.page_size_profile_uuid ||
        binding.state_root.filespace_uuid != croot.filespace_uuid || !binding.maximum_chain_pages ||
        !binding.maximum_index_pages || !binding.maximum_base_pages || !request.transaction_order ||
        !V7(request.transaction_uuid) || request.rows.empty())
      return result;
    for (std::size_t i = 0; i < request.rows.size(); ++i) {
      const auto& row = request.rows[i];
      if (row.role < 1 || row.role > pg::kNativeCatalogRelationRoles || !V7(row.row_uuid) || !V7(row.version_uuid) || row.version_uuid == row.row_uuid)
        return refuse_row(E::invalid_request, i);
    }
    if (binding.read_only_database) return Refuse(E::read_only_database);
    // A name row carries its name and an empty payload (the envelope is
    // encoded once placed); the relation admits the record's kind.
    bool any_entry = false, needs_audit = false, any_comment = false;
    for (std::size_t i = 0; i < request.rows.size(); ++i) {
      const auto& row = request.rows[i];
      const auto& m = row.metadata;
      const bool name = m.record.header.kind == catalog::CatalogRecordKind::localized_name;
      // SB_NATIVE_COMMENT_INSERT: comment rows are data (owner Q79 = 1).
      const bool comment = m.record.header.kind == catalog::CatalogRecordKind::localized_comment;
      any_comment = any_comment || comment;
      if (name != row.name.has_value() || (name && !m.record.payload.empty()) ||
          m.record.header.kind == catalog::CatalogRecordKind::database)
        return refuse_row(E::invalid_insert, i);
      if (!RoleAdmits(row.role, m)) return refuse_row(E::invalid_role, i);
      if (name && !m.record.header.deleted)
        if (const auto text = NameTextRefusal(*row.name)) return refuse_row(*text, i);
      if (name && !catalog::CatalogNamePayloadMatchesMetadata(*row.name, m)) return refuse_row(E::invalid_insert, i);
      any_entry = any_entry || (name && std::holds_alternative<catalog::CatalogNameEntry>(*row.name));
      needs_audit = needs_audit || (!name && !comment && m.record.header.kind != catalog::CatalogRecordKind::audit_evidence);
    }
    // Rows and versions are new within the request; objects too.
    for (std::size_t i = 0; i < request.rows.size(); ++i)
      for (std::size_t j = 0; j < i; ++j) {
        const auto& a = request.rows[i];
        const auto& b = request.rows[j];
        if (a.version_uuid == b.version_uuid || a.version_uuid == b.row_uuid || a.row_uuid == b.version_uuid ||
            (a.role == b.role && a.row_uuid == b.row_uuid))
          return refuse_row(E::duplicate_row, i);
        if (a.role == b.role && a.metadata.record.header.object_uuid.value == b.metadata.record.header.object_uuid.value)
          return refuse_row(E::duplicate_object, i);
      }
    const u32 page_size = profile->page_size_bytes;

    // ---- the transaction ---------------------------------------------------
    pg::TransactionWriterBinding tx;
    tx.database_uuid = binding.database_uuid;
    tx.state_root = binding.state_root;
    tx.maximum_chain_pages = binding.maximum_chain_pages;
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tx);
    if (!chains.ok()) {
      result.error = chains.error == pg::TransactionWriterError::io_failure ? E::io_failure
                     : chains.error == pg::TransactionWriterError::resource_exhausted ? E::resource_exhausted
                                                                                       : E::invalid_inventory;
      result.inventory_error = chains.page_error;
      result.io = chains.io;
      return result;
    }
    const auto built = pg::BuildLocalTransactionInventoryFromImages(page_size, chains.state_images, chains.header_images);
    if (!built.ok()) {
      result.error = E::invalid_inventory;
      result.inventory_error = built.error;
      return result;
    }
    const auto& entries = built.inventory.entries;
    const auto entry = [&](u64 order) -> const mga::TransactionInventoryEntry* {
      const auto at = std::lower_bound(entries.begin(), entries.end(), order,
                                       [](const auto& e, u64 n) { return e.identity.local_id.value < n; });
      return at == entries.end() || at->identity.local_id.value != order ? nullptr : &*at;
    };
    const auto* self = entry(request.transaction_order);
    if (!self || self->identity.transaction_uuid.value != request.transaction_uuid)
      return Refuse(E::transaction_not_active);
    if (self->state == mga::TransactionState::read_only_active) return Refuse(E::read_only_transaction);
    if (self->state != mga::TransactionState::active) return Refuse(E::transaction_not_active);

    // ---- the catalog root and each relation the request names -------------
    std::vector<byte> image;
    if (!ReadPage(sink, page_size, croot.page_number, image, result)) return result;
    const disk::NativePageReference root_ref{croot.filespace_uuid, croot.page_number, croot.page_generation,
                                             croot.page_size_profile_uuid};
    if (!HeaderBinds(image, binding.database_uuid, root_ref, croot.page_type)) return Refuse(E::invalid_catalog);
    const auto root = pg::DecodeNativeCatalogRoot(image);
    if (!root.ok() || root.root->object_uuid != croot.object_uuid) return Refuse(E::invalid_catalog);
    std::map<u16, Relation> relations;
    std::map<u16, pg::NativeCatalogRootReference> heads;
    std::vector<u16> roles;
    for (const auto& row : request.rows) roles.push_back(row.role);
    if (any_entry) roles.push_back(5);  // the profiles and policies an entry names
    if (needs_audit) roles.push_back(6);  // the events a definition cites (W3b-2b)
    for (const auto role : roles) {
      if (relations.contains(role)) continue;
      const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                     [&](const auto& r) { return r.role == role; });
      if (head == root.root->roots.end() || (head->page_type != 6 && head->page_type != 0x200))
        return Refuse(E::invalid_catalog);
      heads[role] = *head;
      if (!ReadRelation(sink, binding, page_size, *head, relations[role], result)) return result;
    }
    // SB_NATIVE_NAME_TREES: the two name trees over the names relation, from
    // page zero (kinds 22 and 23), when the request inserts name rows. Each
    // inserted name row gets its cells in the same careful-write plan as its
    // row: one in the authoritative tree per entry, one in the UUID-to-name
    // tree per name row (an entry's carries its entry UUID only,
    // SB_NATIVE_NAME_ROW_V4; was: with its path's display components). A
    // database without the roots (component fixtures) has no trees to
    // maintain; one root without the other is a damaged catalog.
    std::vector<NameTree> name_trees;
    if (std::any_of(request.rows.begin(), request.rows.end(), [](const auto& r) { return r.name.has_value(); }) &&
        !ReadNameTrees(sink, binding, page_size, image, name_trees, result))
      return result;
    // SB_NATIVE_COMMENT_TREE: the comment tree (kind 24), when the request
    // inserts comment rows: each gets its cell in the same plan, and the
    // duplicate check probes it.
    std::optional<NameTree> comment_tree;
    if (any_comment && !ReadCommentTree(sink, binding, page_size, image, comment_tree, result)) return result;
    // SB_NATIVE_COMMENT_TARGET: every relation a comment's target may live in.
    if (any_comment && !ReadDefinitionRelations(sink, binding, page_size, *root.root, relations, &heads, result))
      return result;
    // ---- each row: checked, placed, indexed --------------------------------
    // SB_CATALOG_WRITER_ROW_INDEX: see the row loop.
    struct RowIndex {
      struct Object {
        bool held = false;
        std::optional<u64> blocker;
      };
      bool built = false;
      std::set<Uuid> ids, tree_rows;
      std::map<Uuid, Object> objects;
    };
    std::map<u16, RowIndex> row_indexes;
    std::map<Uuid, bool> own_definition;  // SB_CATALOG_WRITER_DEFINITION_MEMO
    // The pages a placement changes, by (role, index): their pages as changed.
    std::set<std::pair<u16, std::size_t>> changed_leaves;
    std::map<u16, std::set<std::size_t>> changed_tree;  // per role: tree pages changed or added
    const auto hold = [&](const pg::RowDataRecord& row) {
      const auto* creator = entry(row.local_transaction_id);
      if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value) return Hold::held;
      const auto state = mga::InventoryVisibilityState(*creator);
      if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal)
        return Hold::none;
      return InProgress(state) && row.local_transaction_id != request.transaction_order ? Hold::pending : Hold::held;
    };
    const auto row_index_of = [&](u16 role) -> RowIndex& {
      auto& rx = row_indexes[role];
      if (rx.built) return rx;
      const auto& rel = relations.at(role);
      for (const auto& leaf : rel.leaves)
        for (const auto& row : leaf.page.body.rows) {
          rx.ids.insert(row.row_uuid.value);
          rx.ids.insert(row.version_uuid);
          const auto found = leaf.metadata.find(row.version_uuid);
          if (found == leaf.metadata.end()) continue;
          auto& object = rx.objects[found->second.record.header.object_uuid.value];
          const auto state = hold(row);
          if (state == Hold::held) object.held = true;
          if (state == Hold::pending && !object.blocker) object.blocker = row.local_transaction_id;
        }
      for (const auto& page : rel.tree)
        for (const auto& cell : page.page.cells) rx.tree_rows.insert(cell.key.row_uuid);
      rx.built = true;
      return rx;
    };

    // ---- names: what this transaction sees of profiles, policies, names ---
    std::vector<const catalog::CatalogNamePayload*> request_names;
    for (const auto& row : request.rows)
      if (row.name) request_names.push_back(&*row.name);
    NameState names(binding, request.transaction_order, relations, entry, std::move(request_names));
    if (any_entry && !names.Load()) return Refuse(E::invalid_catalog);
    if (!name_trees.empty()) names.ProbeWith(&name_trees[0].tree);  // SB_NATIVE_NAME_PROBE (N3)
    if (name_trees.size() > 1) names.DefaultIndexWith(&name_trees[1].tree);  // SB_NATIVE_NAME_DEFAULT_INDEX
    bool definitions_read = any_comment;  // ReadDefinitionRelations above

    for (std::size_t i = 0; i < request.rows.size(); ++i) {
      const auto& want = request.rows[i];
      std::optional<catalog::IdentifierFoldMode> entry_fold;  // SB_NATIVE_NAME_ROW_V4: from the entry check
      auto& relation = relations.at(want.role);
      const auto& head = heads.at(want.role);
      const auto& m = want.metadata;
      const auto& h = m.record.header;
      // SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): a definition cites an event
      // of this transaction, in this request or already written by it.
      // (An audit_evidence row is its own anchor; one not of a registered
      // family the codec refuses below.)
      const bool comment = h.kind == catalog::CatalogRecordKind::localized_comment;
      if (!want.name && !comment && m.record.header.kind != catalog::CatalogRecordKind::audit_evidence) {
        const bool in_request =
            m.audit_uuid.kind == UuidKind::object &&
            std::any_of(request.rows.begin(), request.rows.end(), [&](const auto& r) {
              return r.role == 6 && IsChangeEvent(r.metadata) && r.metadata.record.header.object_uuid.value == m.audit_uuid.value;
            });
        if (!in_request && !StoredOwnEvent(relations.at(6), m.audit_uuid, request.transaction_uuid, request.transaction_order))
          return refuse_row(E::unaudited_change, i);
      }
      // A first version by this transaction.
      if ((!want.name && catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(m))) ||
          h.row_uuid.kind != UuidKind::row || h.row_uuid.value != want.row_uuid || h.deleted ||
          m.definition_version != 1 || m.authority_scope == catalog::CatalogAuthorityScope::cluster ||
          m.creator_transaction_uuid.kind != UuidKind::transaction ||
          m.creator_transaction_uuid.value != request.transaction_uuid ||
          m.creator_local_transaction_id != request.transaction_order)
        return refuse_row(E::invalid_insert, i);
      if (AheadOfRoot(m, *root.root)) return refuse_row(E::ahead_of_catalog_root, i);
      // SB_AUDIT_CATALOG_METADATA_CHANGE: an event row is this transaction's
      // (the codec's metadata rule: created by its origin) and this database's.
      if (!catalog::CatalogMetadataChangeMatchesDatabase(catalog::BorrowCatalogMetadataVersion(m), binding.database_uuid) ||
          (catalog::IsCatalogMetadataChangePayload(m.record.payload) &&
           (catalog::DecodeCatalogMetadataChange(m.record.payload).record->origin_transaction_uuid.value !=
                request.transaction_uuid ||
            catalog::DecodeCatalogMetadataChange(m.record.payload).record->origin_local_transaction_id !=
                request.transaction_order)))
        return refuse_row(E::invalid_insert, i);
      // New to the relation: no row or version with these UUIDs; no row
      // holding the object (a hold another transaction in progress has is a
      // write conflict: if it rolls back the object can be inserted).
      // SB_CATALOG_SINGLE_WRITER: checked against images read above and
      // written below with nothing held between; only one catalog writer per
      // database makes the check still true when the pages are written.
      // SB_CATALOG_WRITER_ROW_INDEX (streaming work, 2026-11-28): the same three checks, in the same order and with
      // the same verdicts, from a per-relation index built once per pass and kept current as this request places
      // rows (row and version UUIDs held, per object whether a held version and the first pending creator exist, the
      // row UUIDs of the index's cells). Before this every inserted row scanned every row and cell of the relation.
      {
        auto& rx = row_index_of(want.role);
        if (rx.ids.contains(want.row_uuid) || rx.ids.contains(want.version_uuid)) return refuse_row(E::duplicate_row, i);
        if (const auto at = rx.objects.find(h.object_uuid.value); at != rx.objects.end()) {
          if (at->second.held) return refuse_row(E::duplicate_object, i);
          if (at->second.blocker) {
            result.blocker_order = at->second.blocker;
            return refuse_row(E::write_conflict, i);
          }
        }
        if (rx.tree_rows.contains(want.row_uuid)) return refuse_row(E::duplicate_row, i);
      }

      // SB_NATIVE_COMMENT_INSERT (owner Q82 = 1): one live comment per
      // (target, language): this request's earlier rows, then the stored ones.
      std::optional<catalog::CatalogCommentRecordView> comment_record;
      if (comment) {
        const auto decoded = catalog::DecodeCatalogComment(m.record.payload);
        if (!decoded.ok()) return refuse_row(E::invalid_insert, i);
        comment_record = *decoded.record;
        for (std::size_t j = 0; j < i; ++j) {
          const auto& earlier = request.rows[j].metadata;
          if (earlier.record.header.kind != catalog::CatalogRecordKind::localized_comment) continue;
          const auto other = catalog::DecodeCatalogComment(earlier.record.payload);
          if (other.ok() && other.record->target_object_uuid == comment_record->target_object_uuid &&
              other.record->language_uuid == comment_record->language_uuid)
            return refuse_row(E::duplicate_comment, i);
        }
        // SB_NATIVE_COMMENT_TARGET (owner 2026-11-13, Q86, Q87 = 1): the
        // target is a live definition in this transaction's view, or a row
        // of this request carries it.
        {
          const auto& target = comment_record->target_object_uuid;
          const bool in_request = std::any_of(request.rows.begin(), request.rows.end(), [&](const auto& r) {
            const auto& rh = r.metadata.record.header;
            return rh.object_uuid.value == target && rh.kind != catalog::CatalogRecordKind::localized_name &&
                   rh.kind != catalog::CatalogRecordKind::localized_comment;
          });
          const auto verdict = CheckCommentTarget(relations, target, entry, request.transaction_order, in_request);
          if (verdict.error == E::invalid_catalog) return Refuse(E::invalid_catalog);
          if (verdict.error != E::none) {
            result.blocker_order = verdict.blocker;
            return refuse_row(verdict.error, i);
          }
        }
        const auto pair = HoldOfCommentPair(relation, comment_tree ? &comment_tree->tree : nullptr,
                                            comment_record->target_object_uuid, comment_record->language_uuid, entry,
                                            request.transaction_order);
        if (pair.invalid) return Refuse(E::invalid_catalog);
        if (pair.held) return refuse_row(E::duplicate_comment, i);
        if (pair.blocker) {
          result.blocker_order = pair.blocker;
          return refuse_row(E::write_conflict, i);
        }
      }

      // An entry: its keys, then the names it could collide with.
      // SB_CATALOG_SINGLE_WRITER: like the object check above, this holds only
      // while no other catalog writer runs between the read and the write.
      if (want.name && std::holds_alternative<catalog::CatalogNameEntry>(*want.name)) {
        const auto verdict = names.CheckEntry(*want.name);
        if (verdict.error == E::invalid_catalog) return Refuse(E::invalid_catalog);
        if (verdict.error != E::none) {
          if (verdict.blocker) result.blocker_order = verdict.blocker;
          return refuse_row(verdict.error, i);
        }
        entry_fold = verdict.own_fold;
        // SB_NATIVE_NAME_DEFAULT_INDEX (owner Q228 revisited, Q259): at most
        // one default per (object, language) and one object default.
        const auto& x = std::get<catalog::CatalogNameEntry>(*want.name);
        const auto defaults = names.CheckDefault(x, std::nullopt);
        if (defaults.error == E::invalid_catalog) return Refuse(E::invalid_catalog);
        if (defaults.error != E::none) {
          if (defaults.blocker) result.blocker_order = defaults.blocker;
          return refuse_row(defaults.error, i);
        }
        // SB_NATIVE_NAME_DEFINITION_LOCK (owner Q247 = yes): ADD NAME for an
        // existing object needs this transaction's version of its definition
        // row (a create inserts the definition in the same request).
        const bool created_here = std::any_of(request.rows.begin(), request.rows.end(), [&](const auto& r) {
          return !r.name && r.metadata.record.header.object_uuid.value == x.object_uuid.value &&
                 r.metadata.record.header.kind != catalog::CatalogRecordKind::localized_comment;
        });
        if (!created_here) {
          if (!definitions_read) {
            if (!ReadDefinitionRelations(sink, binding, page_size, *root.root, relations, &heads, result)) return result;
            definitions_read = true;
          }
          // SB_CATALOG_WRITER_DEFINITION_MEMO (streaming work, 2026-11-28): the scan of the definition relations is
          // decided once per object for this pass (the request inserts no row of a definition relation for an object
          // it has not created here, so the answer cannot change inside the pass).
          auto known = own_definition.find(x.object_uuid.value);
          if (known == own_definition.end())
            known = own_definition.emplace(x.object_uuid.value,
                                           OwnDefinitionVersion(relations, x.object_uuid.value, request.transaction_order)).first;
          if (!known->second) return refuse_row(E::definition_not_locked, i);
        }
        names.HoldEntry(*want.name);
      }

      // The row. A name's envelope binds where the row is placed.
      pg::RowDataRecord added;
      added.row_uuid = {UuidKind::row, want.row_uuid};
      added.version_uuid = want.version_uuid;
      added.transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
      added.local_transaction_id = request.transaction_order;
      added.row_version = 1;
      const auto placed = [&](const NativeCatalogLeafPage& page, u32 slot)
          -> std::optional<std::pair<catalog::CatalogMetadataVersion, std::vector<byte>>> {
        auto final_metadata = m;
        if (want.name) {
          const catalog::CatalogNameVersionBinding at{
              {UuidKind::database, page.header.database_uuid}, {UuidKind::filespace, page.header.filespace_uuid},
              {UuidKind::row, want.row_uuid}, {UuidKind::row, want.version_uuid}, h.object_uuid,
              {UuidKind::transaction, request.transaction_uuid}, page.body.page_number, slot, page.header.page_generation,
              1, request.transaction_order, m.catalog_generation};
          const auto envelope = catalog::EncodeCatalogNameEnvelope({at, *want.name});
          if (!envelope.ok()) return std::nullopt;
          final_metadata.record.payload.assign(envelope.bytes.begin(), envelope.bytes.end());
          if (catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(final_metadata)))
            return std::nullopt;
        }
        auto encoded = catalog::EncodeCatalogMetadataVersion(final_metadata);
        if (!encoded.ok()) return std::nullopt;
        return std::pair{std::move(final_metadata), std::move(encoded.bytes)};
      };

      // Placement: the first reached base leaf with room (an unindexed
      // relation has one, its head).
      // SB_CATALOG_WRITER_PLACEMENT_PRECHECK (streaming work, 2026-11-28): the encoded cell, and so the row's footprint,
      // has the same size on every leaf (the name envelope's binding is fixed-width: page, slot and generation are
      // numbers, not lengths), so once the first leaf has been tried the row's footprint is known and a leaf whose
      // recorded body bytes cannot take it is passed over by arithmetic, as RowFits judges it, without encoding the
      // row (a SHA-256 seal and a name envelope per leaf tried). Before this every leaf from the front of the relation
      // was encoded for each inserted row: O(leaves) encodes per row, about half the writer's time at 1,100 rows.
      std::optional<u64> footprint;
      std::optional<std::size_t> target;
      for (std::size_t l = 0; l < relation.leaves.size() && !target; ++l) {
        const auto& candidate = relation.leaves[l].page;
        u32 slot = 0;
        for (const auto& row : candidate.body.rows) slot = std::max(slot, row.stable_slot_id);
        if (slot == std::numeric_limits<u32>::max()) continue;
        if (footprint && candidate.body.free_space_offset && candidate.header.page_size_bytes > 32 &&
            u64{candidate.body.free_space_offset} + *footprint >
                pg::RowDataPageBodyCapacity(candidate.header.page_size_bytes - 32))
          continue;
        auto final_row = placed(candidate, slot + 1);
        if (!final_row) return refuse_row(E::invalid_insert, i);
        auto row = added;
        row.stable_slot_id = slot + 1;
        row.storage_generation = candidate.header.page_generation;
        scratchbird::core::datatypes::DatatypeBinaryValue cell_value;
        cell_value.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
        cell_value.payload = std::move(final_row->second);
        row.cells = {{1, std::move(cell_value), {}}};
        if (!footprint) footprint = pg::RowDataRecordFootprint(row);
        // SB_CATALOG_FREE_SPACE: only a page the row may fit is copied and
        // encoded; the encoder decides.
        if (RowFits(candidate, row) == false) continue;
        auto page = candidate;
        page.body.rows.push_back(std::move(row));
        const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
        if (!fits.ok()) {
          if (fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE") continue;
          if (fits.resource_failure()) return Refuse(E::resource_exhausted);
          return refuse_row(E::invalid_insert, i);
        }
        page.body.free_space_offset = fits.body.free_space_offset;  // SB_CATALOG_FREE_SPACE
        relation.leaves[l].page = std::move(page);
        relation.leaves[l].metadata.emplace(want.version_uuid, std::move(final_row->first));
        target = l;
      }
      // SB_CATALOG_NEW_PAGES (W3b-3a): no reached leaf has room; an indexed
      // relation gets a new base leaf, reached by this row's cell. An
      // unindexed relation has only its head.
      // SB_NATIVE_COMMENT_RELATION: an indexed relation still empty (the
      // comments relation at create: its empty index reaches no leaf) whose
      // identity is a universal constant gets its first base leaf the same way.
      const auto constant = NativeCatalogConstantRelationUuid(want.role);
      NativeCatalogLeafPage first;
      if (relation.leaves.empty() && constant) {
        first.header = {page_size, 6, binding.database_uuid, croot.filespace_uuid, {}, 0, 1, 0, croot.page_size_profile_uuid};
        first.body.relation_uuid = {UuidKind::object, *constant};
        first.body.segment_id = first.body.segment_generation = first.body.page_generation =
            first.body.compaction_generation = 1;
      }
      if (!target && head.page_type == 0x200 && binding.allocation_root && (!relation.leaves.empty() || constant)) {
        const auto& like = relation.leaves.empty() ? first : relation.leaves.front().page;
        const auto taken = pages.Take(6, like.body.relation_uuid.value);
        if (!taken) return pages.disagreed() ? Refuse(E::invalid_catalog) : refuse_row(E::new_pages_exhausted, i);
        auto leaf = NewBaseLeaf(sink, like, page_size, *taken, !pages.dry(), result);
        if (!leaf) return result;
        relation.leaves.push_back(std::move(*leaf));
        relation.bases.push_back({croot.filespace_uuid, taken->page_number, 1, croot.page_size_profile_uuid});
        const auto l = relation.leaves.size() - 1;
        auto page = relation.leaves[l].page;
        auto final_row = placed(page, 1);
        if (!final_row) return refuse_row(E::invalid_insert, i);
        auto row = added;
        row.stable_slot_id = 1;
        row.storage_generation = 1;
        scratchbird::core::datatypes::DatatypeBinaryValue cell_value;
        cell_value.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
        cell_value.payload = std::move(final_row->second);
        row.cells = {{1, std::move(cell_value), {}}};
        page.body.rows.push_back(std::move(row));
        const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
        if (!fits.ok()) {
          if (fits.resource_failure()) return Refuse(E::resource_exhausted);
          return refuse_row(fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE" ? E::relation_full
                                                                                               : E::invalid_insert, i);
        }
        page.body.free_space_offset = fits.body.free_space_offset;  // SB_CATALOG_FREE_SPACE
        relation.leaves[l].page = std::move(page);
        relation.leaves[l].metadata.emplace(want.version_uuid, std::move(final_row->first));
        target = l;
      }
      if (!target) return refuse_row(E::relation_full, i);
      changed_leaves.insert({want.role, *target});
      {
        auto& rx = row_index_of(want.role);  // (SB_CATALOG_WRITER_ROW_INDEX: the placed row is held by this transaction)
        rx.ids.insert(want.row_uuid);
        rx.ids.insert(want.version_uuid);
        rx.objects[h.object_uuid.value].held = true;
      }
      // SB_NATIVE_COMMENT_TREE: the row's cell in the comment tree.
      if (comment && comment_tree) {
        const auto key = catalog::CatalogCommentTreeKey(comment_record->target_object_uuid, comment_record->language_uuid);
        pg::NativeBtreeCell cell;
        cell.key.encoded_key.assign(key.begin(), key.end());
        cell.key.row_uuid = want.row_uuid;
        cell.key.version_uuid = want.version_uuid;
        cell.base_page = relation.bases[*target];
        cell.creator_local_transaction_id = request.transaction_order;
        if (const auto error = InsertTreeCell(sink, comment_tree->tree, comment_tree->changed, std::move(cell),
                                              E::comment_tree_full, E::invalid_insert, pages, page_size,
                                              binding.allocation_root.has_value(), result))
          return refuse_row(*error, i);
      }
      // SB_NATIVE_NAME_TREES: the row's cells in the name trees.
      if (want.name && !name_trees.empty()) {
        const auto payload_view = catalog::BorrowCatalogNamePayload(*want.name);
        const auto add = [&](unsigned t, std::vector<byte> encoded_key, std::vector<byte> payload) -> std::optional<E> {
          pg::NativeBtreeCell cell;
          cell.key.encoded_key = std::move(encoded_key);
          cell.key.row_uuid = want.row_uuid;
          cell.key.version_uuid = want.version_uuid;
          cell.base_page = relation.bases[*target];
          cell.creator_local_transaction_id = request.transaction_order;
          cell.payload = std::move(payload);
          // SB_NATIVE_NAME_KEY_EXCEEDS_PAGE_CAPACITY (owner Q225 = yes): a
          // cell key too long for the page has its own code (was:
          // invalid_insert).
          return InsertTreeCell(sink, name_trees[t].tree, name_trees[t].changed, std::move(cell), E::name_tree_full,
                                E::name_key_exceeds_page_capacity, pages, page_size,
                                binding.allocation_root.has_value(), result);
        };
        if (const auto* v = std::get_if<catalog::CatalogNameVectorView>(&payload_view)) {
          const auto kind = KnownKind(v->object_kind);
          if (!kind) return refuse_row(E::invalid_name, i);
          if (const auto error = add(1, catalog::CatalogNameUuidTreeVectorKey(v->object_uuid.value, *kind),
                                     catalog::CatalogNameUuidTreeVectorPayload(*v)))
            return refuse_row(*error, i);
        } else {
          const auto& e = std::get<catalog::CatalogNameEntryView>(payload_view);
          const auto kind = KnownKind(e.object_kind);
          if (!kind) return refuse_row(E::invalid_name, i);
          // SB_NATIVE_NAME_ROW_V4: the path must walk (nothing of it is
          // stored); the authoritative key is the own lane derived under the
          // entry's profile; the UUID-to-name payload is the entry UUID.
          if (catalog::CheckCatalogNameEntryPath(e, names.Paths()) != catalog::CatalogNamePathError::none)
            return refuse_row(E::invalid_name, i);
          if (!entry_fold) return refuse_row(E::invalid_name, i);
          const catalog::CatalogNameDerivedText own{e.raw_name_text, *entry_fold};
          if (const auto error = add(0, catalog::CatalogNameAuthoritativeTreeKey(e, own, *kind),
                                     catalog::CatalogNameAuthoritativeTreePayload(e)))
            return refuse_row(*error, i);
          if (const auto error = add(1, catalog::CatalogNameUuidTreeEntryKey(e, *kind),
                                     catalog::CatalogNameUuidTreeEntryPayload(e)))
            return refuse_row(*error, i);
        }
      }
      if (head.page_type == 6) continue;

      // The index: the row's cell (SB_NATIVE_BTREE_COMPRESSED_PAGE, owner Q38
      // = 1: it carries the version's creating transaction).
      pg::NativeBtreeCell cell;
      cell.key.encoded_key.assign(want.row_uuid.bytes.begin(), want.row_uuid.bytes.end());
      cell.key.row_uuid = want.row_uuid;
      cell.key.version_uuid = want.version_uuid;
      cell.base_page = relation.bases[*target];
      cell.creator_local_transaction_id = request.transaction_order;
      if (const auto error = InsertTreeCell(sink, relation.tree, changed_tree[want.role], std::move(cell), E::index_full,
                                            E::invalid_insert, pages, page_size, binding.allocation_root.has_value(), result))
        return refuse_row(*error, i);
      row_index_of(want.role).tree_rows.insert(want.row_uuid);
    }

    // ---- the paths (SB_NATIVE_NAME_CASCADE, owner Q48 = 1; since
    // SB_NATIVE_NAME_ROW_V4, owner Q157 = 1) ----------------------------------
    // The request's names may change which name a stored entry's path uses (a
    // new entry marked default_for_language for an object whose entries in
    // that language fell back to its vector's default). Nothing below is
    // rewritten (paths are walked at read); an entry they would leave without
    // a path (an ambiguous scope: a second default in one language) is
    // invalid_name, in this transaction's view (CheckPathsAfterChange; owner
    // Q226: was, Q212, also a write_conflict with another transaction's entry
    // in progress whose path they broke or made walk; Q47: moved or broke).
    // These refusals concern the request as a whole: no failed_row. (Was: each
    // stored entry the request's names moved got a successor version in this
    // write, from cascade_versions.)
    if (any_entry) {
      std::vector<const catalog::CatalogNamePayload*> added;
      for (const auto& row : request.rows)
        if (row.name) added.push_back(&*row.name);
      // (Q212: was also before_paths; see CheckPathsAfterChange.)
      const auto after_paths = names.StoredPaths(std::nullopt, nullptr, added);
      if (auto stop = CheckPathsAfterChange(names, after_paths, {}, E::invalid_name)) return *stop;
    }

    // ---- the images --------------------------------------------------------
    // Pages are kept apart (each image lives until the plan executes).
    struct Planned {
      u64 page_number;
      std::vector<byte> image;
      const std::vector<byte>* as_read;
      int rank = 1;  // SB_CATALOG_SPLIT_LAG write order (TreePage::rank)
      bool linked = false;  // carries a cascade's forward link: written after every other leaf
    };
    std::vector<Planned> leaf_writes, tree_writes;
    for (const auto& [role, l] : changed_leaves) {
      auto& leaf = relations.at(role).leaves[l];
      auto encoded = EncodeNativeCatalogLeaf(leaf.page);
      if (!encoded.ok()) {
        result.error = encoded.error == NativeCatalogLeafError::resource_exhausted ? E::resource_exhausted
                                                                                   : E::invalid_insert;
        result.leaf_error = encoded.error;
        return result;
      }
      leaf_writes.push_back({relations.at(role).bases[l].page_number, std::move(encoded.bytes), &leaf.image, 1,
                             false});  // (was: a cascade's forward link, role 2)
    }
    for (const auto& [role, pages_changed] : changed_tree)
      for (const auto t : pages_changed) {
        auto& page = relations.at(role).tree[t];
        auto encoded = pg::EncodeNativeBtreePage(page.page);
        if (!encoded.ok()) return Refuse(E::invalid_catalog);
        tree_writes.push_back({page.ref.page_number, std::move(encoded.bytes), &page.image, page.rank});
      }
    // SB_NATIVE_NAME_TREES: the name tree pages, after every base leaf like
    // the relation index pages; SB_NATIVE_COMMENT_TREE: the comment tree's too.
    std::vector<NameTree*> trees;
    for (auto& tree : name_trees) trees.push_back(&tree);
    if (comment_tree) trees.push_back(&*comment_tree);
    for (auto* tree : trees)
      for (const auto t : tree->changed) {
        auto& page = tree->tree[t];
        auto encoded = pg::EncodeNativeBtreePage(page.page);
        if (!encoded.ok()) return Refuse(E::invalid_catalog);
        tree_writes.push_back({page.ref.page_number, std::move(encoded.bytes), &page.image, page.rank});
      }

    // SB_CATALOG_OWN_VERSION_REPLACE: the replaced versions' cells, removed
    // and written first. (Built after the images above: a page's image as
    // read becomes its removal image, the main plan's preimage.)
    std::vector<RemovalWrite> removals;
    for (auto* tree : trees)
      if (const auto error = BuildRemovalWrites(*tree, removals)) return Refuse(*error);

    // SB_CATALOG_NEW_PAGES: a dry pass that needs pages stops here, every
    // refusal behind it; the pages are allocated and the real pass writes.
    if (pages.needed()) {
      deferred = true;
      result.error = E::none;
      return result;
    }
    // ---- the writes: every base leaf before every index leaf ----------------
    if (!ExecuteRemovals(sink, binding, page_size, removals, result)) return result;
    // A leaf carrying a cascade's forward link after every other leaf.
    disk::CarefulWritePlan plan;
    std::vector<disk::CarefulWriteNodeId> leaf_nodes, unlinked_nodes;
    for (const bool linked : {false, true})
      for (const auto& w : leaf_writes) {
        if (w.linked != linked) continue;
        const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(w.image),
                                   std::span<const byte>(*w.as_read));
        if (!node.ok()) return Refuse(E::plan_refused);
        if (linked)
          for (const auto before : unlinked_nodes)
            if (plan.Before(before, node.node) != CarefulWriteError::none) return Refuse(E::plan_refused);
        (linked ? leaf_nodes : unlinked_nodes).push_back(node.node);
      }
    leaf_nodes.insert(leaf_nodes.end(), unlinked_nodes.begin(), unlinked_nodes.end());
    // Tree pages after the base leaves, rank by rank (SB_CATALOG_SPLIT_LAG):
    // a page a split added, the split leaf, its old right neighbour, the
    // parent; each rank durable before the next.
    std::vector<disk::CarefulWriteNodeId> earlier = leaf_nodes;
    for (int rank = 0; rank <= 4 * static_cast<int>(NativeCatalogBtreeShape::kMaximumDepth) + 3; ++rank) {
      std::vector<disk::CarefulWriteNodeId> these;
      for (const auto& w : tree_writes) {
        if (w.rank != rank) continue;
        const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(w.image),
                                   std::span<const byte>(*w.as_read));
        if (!node.ok()) return Refuse(E::plan_refused);
        for (const auto before : earlier)
          if (plan.Before(before, node.node) != CarefulWriteError::none) return Refuse(E::plan_refused);
        these.push_back(node.node);
      }
      earlier.insert(earlier.end(), these.begin(), these.end());
    }
    const auto r = plan.Execute(sink, binding.barrier_policy);
    result.write_error = r.error;
    result.io = r.io;
    result.writes += r.written;  // after the removal writes, if any
    result.syncs += r.syncs;
    if (!leaf_writes.empty()) result.page_number = leaf_writes.front().page_number;
    if (r.ok()) {
      result.error = E::none;
      return result;
    }
    result.attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                       r.error == CarefulWriteError::barrier_sync_failed ||
                       r.error == CarefulWriteError::final_sync_failed || r.error == CarefulWriteError::sink_exception;
    result.error = result.attempted                                      ? E::write_failed
                   : r.error == CarefulWriteError::preimage_read_failed ? E::io_failure
                   : r.error == CarefulWriteError::preimage_mismatch    ? E::invalid_catalog
                                                                        : E::plan_refused;
    return result;
  } catch (const std::bad_alloc&) {
    return Refuse(E::resource_exhausted);
  } catch (const std::length_error&) {
    return Refuse(E::resource_exhausted);
  } catch (...) {
    return Refuse(E::invalid_request);
  }
}
}  // namespace

// Runs a write as SB_CATALOG_NEW_PAGES describes: one pass when it needs no
// page; otherwise a dry pass, the allocations, then the real pass.
template <class Request, class Pass>
CatalogVersionWriteResult WithNewPagesCached(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                             const Request& request, Pass pass) {
  try {
    // Placeholder numbers far past any file's end; the real pass uses the
    // allocated ones. Every encoded size is independent of the number.
    PageSource dry(request.new_pages, u64{1} << 32);
    bool deferred = false;
    auto first = pass(sink, binding, request, dry, deferred);
    if (!first.ok() || !deferred) return first;
    CatalogVersionWriteResult allocated;
    if (!AllocateNewPages(sink, binding, request.new_pages, dry.needs(), allocated)) return allocated;
    PageSource real(request.new_pages, allocated.allocated_pages, dry.needs());
    bool again = false;
    auto second = pass(sink, binding, request, real, again);
    second.allocated_pages = allocated.allocated_pages;
    second.writes += allocated.writes;
    second.syncs += allocated.syncs;
    // The passes must agree: the same pages, all of them, and a real write.
    if (second.ok() && (again || real.used() != dry.needs().size())) {
      second.error = E::invalid_catalog;
      second.attempted = true;
    }
    if (!second.ok()) second.attempted = true;  // the allocations were written
    return second;
  } catch (...) {
    return Refuse(E::resource_exhausted);
  }
}

template <class Request, class Pass>
CatalogVersionWriteResult WithNewPages(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                       const Request& request, Pass pass) noexcept {
  try {
    // SB_CATALOG_WRITER_PAGE_CACHE: the caller's cache, or one of this write's
    // own (both passes read the same pages; each relation is read once).
    std::optional<CatalogWriterPageCache> own;
    const CatalogVersionWriterBinding* bound = &binding;
    CatalogVersionWriterBinding with_cache;
    if (!binding.page_cache) {
      own.emplace(binding.maximum_base_pages + binding.maximum_index_pages);
      with_cache = binding;
      with_cache.page_cache = &*own;
      bound = &with_cache;
    }
    return WithNewPagesCached(sink, *bound, request, pass);
  } catch (...) {
    return Refuse(E::resource_exhausted);
  }
}

CatalogVersionWriteResult WriteCatalogInsertVersions(disk::CarefulWriteSink& sink,
                                                     const CatalogVersionWriterBinding& binding,
                                                     const CatalogInsertRequest& request) noexcept {
  return WithNewPages(sink, binding, request, InsertPass);
}

CatalogVersionWriteResult WriteCatalogSuccessorVersion(disk::CarefulWriteSink& sink,
                                                       const CatalogVersionWriterBinding& binding,
                                                       const CatalogVersionRequest& request) noexcept {
  return WithNewPages(sink, binding, request, SuccessorPass);
}

CatalogRootAdvanceResult AdvanceCatalogRoot(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                            const CatalogRootAdvanceRequest& request) noexcept {
  CatalogRootAdvanceResult result;
  const auto refuse = [&](E error) {
    result.error = error;
    return result;
  };
  try {
    const auto& croot = binding.catalog_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(croot.page_size_profile_uuid);
    if (!V7(binding.database_uuid) || (croot.kind != 2 && croot.kind != 8) || !croot.page_number ||
        !croot.page_generation || !profile || binding.state_root.kind != 4 ||
        binding.state_root.page_size_profile_uuid != croot.page_size_profile_uuid ||
        binding.state_root.filespace_uuid != croot.filespace_uuid || !binding.maximum_chain_pages ||
        !request.transaction_order || !V7(request.transaction_uuid) || request.catalog_generation < 2)
      return result;
    if (binding.read_only_database) return refuse(E::read_only_database);
    const u32 page_size = profile->page_size_bytes;

    // The transaction has committed: its state word says so.
    pg::TransactionWriterBinding tx;
    tx.database_uuid = binding.database_uuid;
    tx.state_root = binding.state_root;
    tx.maximum_chain_pages = binding.maximum_chain_pages;
    const auto chains = pg::ReadTransactionChainsThroughSink(sink, tx);
    if (!chains.ok()) {
      result.error = chains.error == pg::TransactionWriterError::io_failure ? E::io_failure
                     : chains.error == pg::TransactionWriterError::resource_exhausted ? E::resource_exhausted
                                                                                       : E::invalid_inventory;
      result.inventory_error = chains.page_error;
      result.io = chains.io;
      return result;
    }
    const auto built = pg::BuildLocalTransactionInventoryFromImages(page_size, chains.state_images, chains.header_images);
    if (!built.ok()) {
      result.error = E::invalid_inventory;
      result.inventory_error = built.error;
      return result;
    }
    const auto& entries = built.inventory.entries;
    const auto at = std::lower_bound(entries.begin(), entries.end(), request.transaction_order,
                                     [](const auto& e, u64 n) { return e.identity.local_id.value < n; });
    if (at == entries.end() || at->identity.local_id.value != request.transaction_order ||
        at->identity.transaction_uuid.value != request.transaction_uuid ||
        !mga::HasCommittedInventoryOutcome(*at))
      return refuse(E::transaction_not_committed);

    // The root as it stands.
    CatalogVersionWriteResult read;
    std::vector<byte> image;
    if (!ReadPage(sink, page_size, croot.page_number, image, read)) {
      result.error = read.error;
      result.io = read.io;
      return result;
    }
    const disk::NativePageReference ref{croot.filespace_uuid, croot.page_number, croot.page_generation,
                                        croot.page_size_profile_uuid};
    if (!HeaderBinds(image, binding.database_uuid, ref, croot.page_type)) return refuse(E::invalid_catalog);
    const auto decoded = pg::DecodeNativeCatalogRoot(image);
    if (!decoded.ok() || decoded.root->object_uuid != croot.object_uuid) return refuse(E::invalid_catalog);
    const auto& now = *decoded.root;
    result.previous_generation = now.catalog_generation;
    if (now.catalog_generation == std::numeric_limits<u64>::max() ||
        request.catalog_generation != now.catalog_generation + 1 || request.schema_epoch < now.schema_epoch ||
        request.security_epoch < now.security_epoch || request.resource_epoch < now.resource_epoch)
      return refuse(E::root_regression);

    auto next = now;
    next.catalog_generation = request.catalog_generation;
    next.schema_epoch = request.schema_epoch;
    next.security_epoch = request.security_epoch;
    next.resource_epoch = request.resource_epoch;
    next.predecessor.reset();
    next.predecessor_sha256.fill(0);
    const auto encoded = pg::EncodeNativeCatalogRoot(next);
    if (!encoded.ok() || encoded.bytes.size() != image.size()) return refuse(E::invalid_catalog);
    // Every byte that changes (generation and epochs at 152, the predecessor
    // at 224, the seal at 304) lies in the page's first 512-byte unit; only
    // that unit is written, so with sector-atomic writes a crash leaves the
    // old root or the new one, and a tear inside the unit fails the seal.
    constexpr std::size_t kUnitBytes = 512;
    if (!std::equal(encoded.bytes.begin() + kUnitBytes, encoded.bytes.end(), image.begin() + kUnitBytes))
      return refuse(E::invalid_catalog);

    disk::CarefulWritePlan plan;
    const auto node = plan.Add(croot.page_number * u64{page_size},
                               std::span<const byte>(encoded.bytes).first(kUnitBytes),
                               std::span<const byte>(image).first(kUnitBytes));
    if (!node.ok()) return refuse(E::plan_refused);
    // The route roots: each at most the catalog root's current values (a
    // crash may have left it behind), brought to the new ones after it.
    std::vector<std::pair<std::vector<byte>, std::vector<byte>>> routes;  // (new, as read)
    routes.reserve(2);  // no reallocation once a span of it is planned
    for (const auto* route : {&binding.configuration_root, &binding.security_root}) {
      if (!*route) continue;
      const auto& at = **route;
      const u16 kind = route == &binding.configuration_root ? 6 : 7;
      if (at.kind != kind || at.page_type != static_cast<u32>(4 + kind) || at.filespace_uuid != croot.filespace_uuid ||
          at.page_size_profile_uuid != croot.page_size_profile_uuid || !at.page_number || !at.page_generation)
        return refuse(E::invalid_request);
      std::vector<byte> current;
      if (!ReadPage(sink, page_size, at.page_number, current, read)) {
        result.error = read.error;
        result.io = read.io;
        return result;
      }
      const disk::NativePageReference route_ref{at.filespace_uuid, at.page_number, at.page_generation,
                                                at.page_size_profile_uuid};
      if (!HeaderBinds(current, binding.database_uuid, route_ref, at.page_type)) return refuse(E::invalid_catalog);
      const auto was = pg::DecodeNativeCatalogRoot(current);
      if (!was.ok() || was.root->root_kind != kind || was.root->object_uuid != at.object_uuid || was.root->predecessor ||
          was.root->catalog_generation > now.catalog_generation || was.root->schema_epoch > now.schema_epoch ||
          was.root->security_epoch > now.security_epoch || was.root->resource_epoch > now.resource_epoch)
        return refuse(E::invalid_catalog);
      auto level = *was.root;
      level.catalog_generation = request.catalog_generation;
      level.schema_epoch = request.schema_epoch;
      level.security_epoch = request.security_epoch;
      level.resource_epoch = request.resource_epoch;
      const auto image_of = pg::EncodeNativeCatalogRoot(level);
      if (!image_of.ok() || image_of.bytes.size() != current.size() ||
          !std::equal(image_of.bytes.begin() + kUnitBytes, image_of.bytes.end(), current.begin() + kUnitBytes))
        return refuse(E::invalid_catalog);
      routes.emplace_back(image_of.bytes, std::move(current));
      const auto& [fresh, old] = routes.back();
      const auto route_node = plan.Add(at.page_number * u64{page_size}, std::span<const byte>(fresh).first(kUnitBytes),
                                       std::span<const byte>(old).first(kUnitBytes));
      if (!route_node.ok() || plan.Before(node.node, route_node.node) != CarefulWriteError::none)
        return refuse(E::plan_refused);
    }
    const auto r = plan.Execute(sink, binding.barrier_policy);
    result.write_error = r.error;
    result.io = r.io;
    result.writes = r.written;
    result.syncs = r.syncs;
    if (r.ok()) {
      result.error = E::none;
      return result;
    }
    result.attempted = r.written > 0 || r.syncs > 0 || r.error == CarefulWriteError::write_failed ||
                       r.error == CarefulWriteError::barrier_sync_failed ||
                       r.error == CarefulWriteError::final_sync_failed || r.error == CarefulWriteError::sink_exception;
    result.error = result.attempted                                      ? E::write_failed
                   : r.error == CarefulWriteError::preimage_read_failed ? E::io_failure
                   : r.error == CarefulWriteError::preimage_mismatch    ? E::invalid_catalog
                                                                        : E::plan_refused;
    return result;
  } catch (const std::bad_alloc&) {
    return refuse(E::resource_exhausted);
  } catch (const std::length_error&) {
    return refuse(E::resource_exhausted);
  } catch (...) {
    return refuse(E::invalid_request);
  }
}

// SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2b): a transaction's event, built
// from the create's construction intent row (owner, owning schema, parent)
// and written through the insert path.
CatalogVersionWriteResult WriteCatalogChangeEvent(disk::CarefulWriteSink& sink, const CatalogVersionWriterBinding& binding,
                                                  const CatalogChangeEventRequest& request) noexcept {
  CatalogVersionWriteResult result;
  try {
    const auto& croot = binding.catalog_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(croot.page_size_profile_uuid);
    if (!profile || !croot.page_number || !V7(binding.database_uuid)) return result;
    const u32 page_size = profile->page_size_bytes;
    std::vector<byte> image;
    if (!ReadPage(sink, page_size, croot.page_number, image, result)) return result;
    const disk::NativePageReference root_ref{croot.filespace_uuid, croot.page_number, croot.page_generation,
                                             croot.page_size_profile_uuid};
    if (!HeaderBinds(image, binding.database_uuid, root_ref, croot.page_type)) return Refuse(E::invalid_catalog);
    const auto root = pg::DecodeNativeCatalogRoot(image);
    if (!root.ok() || root.root->object_uuid != croot.object_uuid) return Refuse(E::invalid_catalog);
    const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(), [](const auto& r) { return r.role == 6; });
    if (head == root.root->roots.end() || (head->page_type != 6 && head->page_type != 0x200)) return Refuse(E::invalid_catalog);
    Relation events;
    if (!ReadRelation(sink, binding, page_size, *head, events, result)) return result;
    const catalog::CatalogMetadataVersion* intent = nullptr;
    for (const auto& leaf : events.leaves)
      for (const auto& [version, m] : leaf.metadata)
        if (!intent && m.object_subtype == scratchbird::core::catalog::CatalogObjectSubtype::node_construction_intent && catalog::IsCatalogConstructionIntentPayload(m.record.payload))
          intent = &m;
    if (!intent) return Refuse(E::invalid_catalog);
    catalog::CatalogMetadataChange p;
    p.event_uuid = request.event_uuid;
    p.database_uuid = binding.database_uuid;
    p.engine_uuid = request.engine_uuid;
    p.origin_transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
    p.origin_local_transaction_id = request.transaction_order;
    p.event_time_utc_us = request.event_time_utc_us;
    p.catalog_generation = root.root->catalog_generation;
    p.event = catalog::MetadataChangeAuditEvent::metadata_change;
    p.event_class = catalog::MetadataChangeAuditClass::metadata_change;
    p.policy_decision = catalog::MetadataChangeAuditDecision::not_evaluated;
    p.redaction = catalog::MetadataChangeAuditRedaction::safe_message_only;
    p.actor_uuid = request.actor_uuid;
    std::array<byte, catalog::kCatalogMetadataChangeMaxBytes> out{};
    const auto encoded = catalog::EncodeCatalogMetadataChangeInto(p, out);
    if (!encoded.ok()) return Refuse(E::invalid_insert);
    auto m = *intent;
    m.record.header.object_uuid = {UuidKind::object, request.event_uuid};
    m.record.header.row_uuid = {UuidKind::row, request.row_uuid};
    m.record.header.deleted = false;
    m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size());
    m.object_subtype = scratchbird::core::catalog::CatalogObjectSubtype::catalog_metadata_change;
    m.audit_uuid = {UuidKind::object, request.event_uuid};
    m.creator_transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
    m.creator_local_transaction_id = request.transaction_order;
    m.definition_version = 1;
    m.catalog_generation = root.root->catalog_generation;
    CatalogInsertRequest insert;
    insert.transaction_order = request.transaction_order;
    insert.transaction_uuid = request.transaction_uuid;
    insert.rows.push_back({6, request.row_uuid, request.version_uuid, std::move(m), std::nullopt});
    insert.new_pages = request.new_pages;
    return WriteCatalogInsertVersions(sink, binding, insert);
  } catch (...) {
    return Refuse(E::resource_exhausted);
  }
}
}  // namespace scratchbird::storage::database
