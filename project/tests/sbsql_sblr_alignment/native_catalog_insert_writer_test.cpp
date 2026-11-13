// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_CATALOG_INSERT_WRITER
// In-place writes W3b-1a: a transaction inserts the first versions of new
// catalog rows, in place, on databases written by the production create, one
// page size per run (--profile N). The databases, transactions, relation read
// and committed reads are those of native_catalog_version_writer_test (W3a),
// included below.
//
// The rows inserted here are MECHANICS FIXTURES: generic policy rows with no
// name references. The owner rule is that every native object is named; name
// rows come with W3b-1b, and the catalog-change audit event with W3b-2, so a
// database holding these rows does not pass startup catalog verification yet
// (the writer is checked here, not the verifiers).
//
// The oracle, independent of the writer: the relation as read here; the base
// leaf a row takes (the lowest-numbered reached leaf with room by the create's
// own row-size rule); the index leaf whose [low, high) fence range holds the
// key, by a byte-order comparison written here; the cell at the key's
// position; the expected pages encoded by the production codecs from the
// expected rows and cells; every other byte of the file unchanged.
//   - One insert by a transaction in progress, committed, rolled back, dead at
//     restart: the committed read returns the row only once committed; a
//     rolled-back or dead row's object may be inserted again.
//   - An object another transaction in progress inserted: write_conflict
//     naming it; insertable once it rolls back; a duplicate once it commits.
//   - Keys: after the largest key, between two keys inside a leaf, below the
//     smallest (inserted into a root leaf; index_full in a multi-leaf tree,
//     whose leftmost leaf is full on a fresh database).
//   - A batch over two relations: every base leaf written before every index
//     leaf.
//   - An unindexed relation: the head leaf, one write, no index.
//   - Refusals before any write, the file unchanged.
//   - Filling a relation's room: relation_full with no write.
//   - Process loss after each write and each write torn at its half-way point;
//     the index leaf's write torn at the sector before its last change;
//     every read, write and sync failed in turn.
#define SB_CATALOG_VERSION_WRITER_TEST_WITHOUT_MAIN
#include "native_catalog_version_writer_test.cpp"
#include "catalog_name_envelope.hpp"
#include "catalog_name_path.hpp"
#include "catalog_name_tree_keys.hpp"
#include "catalog_records.hpp"
#include "filespace_page_zero.hpp"
#include <deque>

#include <compare>

namespace {

// ---- Keys, compared here (not by the codec) --------------------------------------
pg::NativeBtreeKey KeyOf(const Uuid& row, const Uuid& version) {
  pg::NativeBtreeKey k;
  k.encoded_key.assign(row.bytes.begin(), row.bytes.end());
  k.row_uuid = row;
  k.version_uuid = version;
  return k;
}
int KeyOrder(const pg::NativeBtreeKey& a, const pg::NativeBtreeKey& b) {
  const auto bytes = [](const auto& x, const auto& y) {
    const auto c = std::lexicographical_compare_three_way(x.begin(), x.end(), y.begin(), y.end());
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  };
  if (const int c = bytes(a.encoded_key, b.encoded_key)) return c;
  if (const int c = bytes(a.row_uuid.bytes, b.row_uuid.bytes)) return c;
  return bytes(a.version_uuid.bytes, b.version_uuid.bytes);
}
u64 Ms(const Uuid& id) {
  u64 ms = 0;
  for (int i = 0; i < 6; ++i) ms = (ms << 8) | id.bytes[i];
  return ms;
}
// A UUIDv7 just above `a` (same millisecond, the random tail plus one), if it
// still sorts below `b`.
std::optional<Uuid> Between(const Uuid& a, const Uuid& b) {
  Uuid c = a;
  for (int i = 15; i >= 9; --i)
    if (++c.bytes[i] != 0) break;
  if (KeyOrder(KeyOf(a, a), KeyOf(c, c)) >= 0 || KeyOrder(KeyOf(c, c), KeyOf(b, b)) >= 0) return std::nullopt;
  return c;
}

// ---- The index as read here ---------------------------------------------------------
struct TreeSeen {
  struct Page {
    u64 number = 0;
    pg::NativeBtreePage page;
    std::optional<std::size_t> parent;  // the page the walk reached it from (owner Q46 = 1)
  };
  std::vector<Page> pages;
  std::vector<std::size_t> leaves;  // level-0 pages in key order
  bool root_is_leaf = false;
};
// The tree rooted at page `root_page`.
TreeSeen ReadTreeAt(const Db& b, std::span<const byte> file, u64 root_page) {
  TreeSeen out;
  std::vector<std::pair<u64, std::optional<std::size_t>>> stack{{root_page, std::nullopt}};
  while (!stack.empty()) {
    const auto [n, parent] = stack.back();
    stack.pop_back();
    auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
    Check(page.ok(), "an index page decodes");
    const auto index = out.pages.size();
    if (page.page->first_child) stack.push_back({page.page->first_child->page_number, index});
    for (const auto& cell : page.page->cells)
      if (cell.child) stack.push_back({cell.child->page_number, index});
    if (!page.page->tree_level) {
      out.leaves.push_back(index);
      if (n == root_page) out.root_is_leaf = true;
    }
    out.pages.push_back({n, std::move(*page.page), parent});
  }
  std::sort(out.leaves.begin(), out.leaves.end(), [&](std::size_t x, std::size_t y) {
    const auto& a = out.pages[x].page.low_fence;
    const auto& c = out.pages[y].page.low_fence;
    if (!a) return bool(c);
    if (!c) return false;
    return KeyOrder(*a, *c) < 0;
  });
  return out;
}
TreeSeen ReadTree(const Db& b, std::span<const byte> file) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                 [&](const auto& r) { return r.role == b.role; });
  Check(head != root.root->roots.end() && head->page_type == 0x200, "an indexed relation head");
  return ReadTreeAt(b, file, head->page.page_number);
}
std::size_t LeafFor(const TreeSeen& t, const pg::NativeBtreeKey& key) {
  for (const auto i : t.leaves) {
    const auto& p = t.pages[i].page;
    if (p.low_fence && KeyOrder(key, *p.low_fence) < 0) continue;
    if (p.high_fence && KeyOrder(key, *p.high_fence) >= 0) continue;
    return i;
  }
  throw std::runtime_error("no leaf holds the key");
}
// Room on an index leaf for more cells like its last (row-UUID keys, a
// creator and a base page in this filespace): cells added just above its
// largest key, one at a time, until the production encoder refuses the page
// (SBBTP002 cells are prefix-compressed and vary in size). Capped at 64.
std::size_t FreeCells(const Db& b, std::span<const byte> file, u64 page) {
  const auto image = PageOf(file, page, b.size);
  auto decoded = pg::DecodeNativeBtreePage(std::vector<byte>(image.begin(), image.end()));
  Check(decoded.ok(), "an index page decodes");
  auto leaf = std::move(*decoded.page);
  if (leaf.tree_level || leaf.cells.empty()) return 0;
  std::size_t n = 0;
  auto next = leaf.cells.back();
  while (n < 64) {
    for (int i = 15; i >= 9; --i)
      if (++next.key.version_uuid.bytes[i]) break;
    leaf.cells.push_back(next);
    if (!pg::EncodeNativeBtreePage(leaf).ok()) break;
    ++n;
  }
  return n;
}

// ---- Rows (mechanics fixtures) -------------------------------------------------------
// A generic policy row; owning schema, parent, owner, audit and security policy
// copied from a row of the relation; no name references (names: W3b-1b).
c::CatalogMetadataVersion FixtureRow(const Db& b, std::span<const byte> file, u16 role, const Uuid& object,
                                     const Uuid& row, u64 n) {
  RoleScope scope(b, role);
  const auto rel = ReadRelation(b, file);
  std::optional<c::CatalogMetadataVersion> like;
  for (const auto& l : rel.leaves)
    for (const auto& [version, m] : l.metadata)
      if (!like && !m.owning_schema_uuid.value.is_nil() && !m.record.header.parent_uuid.value.is_nil()) like = m;
  Check(like.has_value(), "a row with an owning schema to copy");
  auto m = f::Metadata(0, {UuidKind::transaction, TxId(n)}, like->owning_schema_uuid.value,
                       "W3B-1A unnamed catalog insert mechanics fixture");
  m.record.header.object_uuid = {UuidKind::object, object};
  m.record.header.row_uuid = {UuidKind::row, row};
  m.record.header.parent_uuid = like->record.header.parent_uuid;
  m.owner_uuid = like->owner_uuid;
  m.audit_uuid = EventRef(n);  // W3b-2b: the transaction's catalog.metadata_change event
  m.security_policy_uuid = like->security_policy_uuid;
  m.owning_schema_uuid = like->owning_schema_uuid;
  m.default_name_uuid = {};
  m.name_vector_uuid = {};
  m.creator_local_transaction_id = n;
  m.object_subtype = scratchbird::core::catalog::CatalogObjectSubtype::schema_application;
  return m;
}
db::CatalogInsertRow Row(const Db& b, std::span<const byte> file, u16 role, const Uuid& row, unsigned version_k,
                         u64 n) {
  db::CatalogInsertRow r;
  r.role = role;
  r.row_uuid = row;
  r.version_uuid = VersionId(version_k);
  r.metadata = FixtureRow(b, file, role, V7(Ms(row), 0x700000 + version_k), row, n);
  return r;
}
db::CatalogInsertRequest Request(u64 n, std::vector<db::CatalogInsertRow> rows) {
  db::CatalogInsertRequest r;
  r.transaction_order = n;
  r.transaction_uuid = TxId(n);
  r.rows = std::move(rows);
  return r;
}

// ---- The oracle -------------------------------------------------------------------------
// The file after the request: each row in turn on the lowest-numbered reached
// leaf with room, its cell at its key's position in the index leaf that holds
// the key. nullopt when a row has no room or its index leaf none.
// SB_CATALOG_LEAF_SPLIT, SB_CATALOG_ROOT_LEAF_SPLIT, SB_CATALOG_BRANCH_SPLIT
// (W3b-3a-2, W3b-3b), as the writer's rule states them, written here over the
// file: `entry` goes into the leaf of the tree rooted at `root_page` whose
// fences hold its key. Without room, and with pages left in `new_pages`
// (taken in order, each the system's: no creator, generation 1, the request's
// next page UUID):
//  - a leaf that is not the root splits at the first point both sides of
//    which encode (all old cells left and the new one alone right when the new
//    one is last, else the middle): the right part on a new leaf; the old
//    leaf's high fence and right link; the new leaf's fences and links; the
//    old right neighbour's left link; a separator into the parent;
//  - a root that is the only page keeps its page over two new leaves;
//  - a full branch splits at its middle separator (a new branch takes that
//    separator's child as first child and the separators after it; the key
//    goes up); a full root branch keeps its page over two new branches.
// A page's parent is the page the walk reached it from (owner Q46 = 1).
// False when the write cannot place it.
struct OracleTree {
  const Db& b;
  std::vector<byte>& file;
  TreeSeen tree;
  std::span<const u64> new_pages;
  std::span<const db::CatalogNewPageIdentity> ids;
  std::size_t& taken;
  std::set<std::size_t> changed;
  d::NativePageReference Ref(std::size_t i) const {
    return {b.primary, tree.pages[i].number, tree.pages[i].page.header.page_generation, b.profile};
  }
  std::optional<std::size_t> Find(u64 number) const {
    for (std::size_t i = 0; i < tree.pages.size(); ++i)
      if (tree.pages[i].number == number) return i;
    return std::nullopt;
  }
  // A new page like `like`, of `type`, emptied; nullopt when no page is left.
  std::optional<std::size_t> New(const pg::NativeBtreePage& like, u32 type, std::optional<std::size_t> parent) {
    if (taken >= new_pages.size()) return std::nullopt;
    auto page = like;
    page.header.page_type = type;
    page.header.page_uuid = ids[taken].page_uuid;
    page.header.page_number = new_pages[taken];
    page.header.page_generation = 1;
    page.creator_transaction_uuid = {};
    page.creator_local_transaction_id = 0;
    page.parent.reset();
    page.left.reset();
    page.right.reset();
    page.low_fence.reset();
    page.high_fence.reset();
    page.first_child.reset();
    page.cells.clear();
    tree.pages.push_back({new_pages[taken], std::move(page), parent});
    ++taken;
    changed.insert(tree.pages.size() - 1);
    return tree.pages.size() - 1;
  }
  bool Separator(std::size_t at, const pg::NativeBtreeKey& key, std::size_t child) {
    auto page = tree.pages[at].page;
    std::size_t slot = 0;
    while (slot < page.cells.size() && KeyOrder(page.cells[slot].key, key) < 0) ++slot;
    pg::NativeBtreeCell separator;
    separator.key = key;
    separator.child = Ref(child);
    page.cells.insert(page.cells.begin() + static_cast<std::ptrdiff_t>(slot), std::move(separator));
    if (pg::EncodeNativeBtreePage(page).ok()) {
      tree.pages[at].page = std::move(page);
      tree.pages[child].parent = at;
      changed.insert(at);
      return true;
    }
    const auto cells = page.cells;
    if (cells.size() < 3) return false;
    const auto m = cells.size() / 2;
    const auto level = page.tree_level;
    if (page.header.page_type == 0x200) {
      const auto l = New(page, 0x201, at);
      if (!l) return false;
      const auto r = New(page, 0x201, at);
      if (!r) return false;
      auto& left = tree.pages[*l].page;
      left.tree_level = level;
      left.parent = Ref(at);
      left.first_child = page.first_child;
      left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
      left.high_fence = cells[m].key;
      left.right = Ref(*r);
      auto& right = tree.pages[*r].page;
      right.tree_level = level;
      right.parent = Ref(at);
      right.first_child = cells[m].child;
      right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m) + 1, cells.end());
      right.low_fence = cells[m].key;
      right.left = Ref(*l);
      auto root = page;
      root.tree_level = static_cast<u16>(level + 1);
      root.first_child = Ref(*l);
      pg::NativeBtreeCell up;
      up.key = cells[m].key;
      up.child = Ref(*r);
      root.cells = {std::move(up)};
      if (!pg::EncodeNativeBtreePage(tree.pages[*l].page).ok() || !pg::EncodeNativeBtreePage(tree.pages[*r].page).ok() ||
          !pg::EncodeNativeBtreePage(root).ok())
        return false;
      tree.pages[at].page = std::move(root);
      changed.insert(at);
      return true;
    }
    const auto parent = tree.pages[at].parent;
    Check(parent.has_value(), "a branch below the root has a parent");
    const std::optional<std::size_t> neighbour = page.right ? Find(page.right->page_number) : std::nullopt;
    const auto r = New(page, 0x201, parent);
    if (!r) return false;
    auto& right = tree.pages[*r].page;
    right.tree_level = level;
    right.parent = page.parent;
    right.first_child = cells[m].child;
    right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m) + 1, cells.end());
    right.low_fence = cells[m].key;
    right.high_fence = page.high_fence;
    right.left = Ref(at);
    right.right = page.right;
    auto left = page;
    left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
    left.high_fence = cells[m].key;
    left.right = Ref(*r);
    if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(tree.pages[*r].page).ok()) return false;
    const auto up_key = cells[m].key;
    tree.pages[at].page = std::move(left);
    changed.insert(at);
    if (neighbour) {
      tree.pages[*neighbour].page.left = Ref(*r);
      changed.insert(*neighbour);
    }
    return Separator(*parent, up_key, *r);
  }
  bool Insert(pg::NativeBtreeCell entry) {
    const auto li = LeafFor(tree, entry.key);
    std::size_t at = 0;
    while (at < tree.pages[li].page.cells.size() && KeyOrder(tree.pages[li].page.cells[at].key, entry.key) < 0) ++at;
    auto cells = tree.pages[li].page.cells;
    cells.insert(cells.begin() + static_cast<std::ptrdiff_t>(at), entry);
    {
      auto page = tree.pages[li].page;
      page.cells = cells;
      if (pg::EncodeNativeBtreePage(page).ok()) {
        tree.pages[li].page = std::move(page);
        changed.insert(li);
        return true;
      }
    }
    std::vector<std::size_t> points;
    if (at + 1 == cells.size()) points.push_back(cells.size() - 1);
    points.push_back(cells.size() / 2);
    const auto old = tree.pages[li].page;
    if (old.header.page_type == 0x200 && old.tree_level == 0) {
      const auto l = New(old, 0x202, li);
      if (!l) return false;
      const auto r = New(old, 0x202, li);
      if (!r) return false;
      for (const auto m : points) {
        if (m == 0 || m >= cells.size()) continue;
        auto left = tree.pages[*l].page, right = tree.pages[*r].page;
        left.parent = right.parent = Ref(li);
        left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
        right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m), cells.end());
        left.high_fence = cells[m].key;
        right.low_fence = cells[m].key;
        left.right = Ref(*r);
        right.left = Ref(*l);
        if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(right).ok()) continue;
        auto root = old;
        root.tree_level = 1;
        root.first_child = Ref(*l);
        pg::NativeBtreeCell separator;
        separator.key = cells[m].key;
        separator.child = Ref(*r);
        root.cells = {separator};
        tree.pages[*l].page = std::move(left);
        tree.pages[*r].page = std::move(right);
        tree.pages[li].page = std::move(root);
        changed.insert(li);
        return true;
      }
      return false;
    }
    if (old.header.page_type != 0x202 || !tree.pages[li].parent) return false;
    const auto parent = *tree.pages[li].parent;
    const std::optional<std::size_t> neighbour = old.right ? Find(old.right->page_number) : std::nullopt;
    const auto n = New(old, 0x202, parent);
    if (!n) return false;
    for (const auto m : points) {
      if (m == 0 || m >= cells.size()) continue;
      auto left = old;
      left.cells.assign(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(m));
      left.right = Ref(*n);
      left.high_fence = cells[m].key;
      auto right = tree.pages[*n].page;
      right.parent = old.parent;
      right.cells.assign(cells.begin() + static_cast<std::ptrdiff_t>(m), cells.end());
      right.left = Ref(li);
      right.right = old.right;
      right.low_fence = cells[m].key;
      right.high_fence = old.high_fence;
      if (!pg::EncodeNativeBtreePage(left).ok() || !pg::EncodeNativeBtreePage(right).ok()) continue;
      tree.pages[li].page = std::move(left);
      tree.pages[*n].page = std::move(right);
      changed.insert(li);
      if (neighbour) {
        tree.pages[*neighbour].page.left = Ref(*n);
        changed.insert(*neighbour);
      }
      return Separator(parent, cells[m].key, *n);
    }
    return false;
  }
};
bool OracleInsertCell(const Db& b, std::vector<byte>& file, u64 root_page, pg::NativeBtreeCell entry,
                      std::span<const u64> new_pages, std::span<const db::CatalogNewPageIdentity> ids, std::size_t& taken) {
  OracleTree oracle{b, file, ReadTreeAt(b, file, root_page), new_pages, ids, taken, {}};
  if (!oracle.Insert(std::move(entry))) return false;
  for (const auto i : oracle.changed) {
    const auto image = pg::EncodeNativeBtreePage(oracle.tree.pages[i].page);
    Check(image.ok(), "the expected tree page encodes");
    std::copy(image.bytes.begin(), image.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(oracle.tree.pages[i].number * b.size));
  }
  return true;
}
// SB_CATALOG_NEW_PAGES (W3b-3a): `new_pages`, the pages the writer allocated,
// taken in order when no reached leaf of an indexed relation has room: a new
// leaf is a sibling's header and body identity with that page's number, the
// request's next page UUID and generation 1.
std::optional<std::vector<byte>> InsertOracle(const Db& b, std::span<const byte> before, const db::CatalogInsertRequest& r,
                                              std::span<const u64> new_pages = {}) {
  std::size_t taken = 0;
  std::vector<byte> file(before.begin(), before.end());
  const auto put = [&](u64 page, std::span<const byte> image) {
    std::copy(image.begin(), image.end(), file.begin() + static_cast<std::ptrdiff_t>(page * b.size));
  };
  for (const auto& want : r.rows) {
    RoleScope scope(b, want.role);
    const auto rel = ReadRelation(b, file);
    // A name row's payload is its envelope, binding where it is placed.
    const auto finished = [&](const LeafSeen& l) {
      auto m = want.metadata;
      if (want.name) {
        u32 slot = 0;
        for (const auto& x : l.leaf.body.rows) slot = std::max(slot, x.stable_slot_id);
        const c::CatalogNameVersionBinding at{
            {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
            {UuidKind::row, want.row_uuid}, {UuidKind::row, want.version_uuid}, m.record.header.object_uuid,
            {UuidKind::transaction, r.transaction_uuid}, l.page, slot + 1, l.leaf.header.page_generation, 1,
            r.transaction_order, m.catalog_generation};
        const auto envelope = c::EncodeCatalogNameEnvelope({at, *want.name});
        Check(envelope.ok(), "the expected name envelope encodes");
        m.record.payload.assign(envelope.bytes.begin(), envelope.bytes.end());
      }
      return m;
    };
    const LeafSeen* target = nullptr;
    for (const auto& l : rel.leaves)
      if (!target && l.leaf.body.free_space_bytes >= RowBytes(finished(l))) target = &l;
    LeafSeen fresh;
    if (!target && rel.indexed && taken < new_pages.size()) {
      fresh.page = new_pages[taken];
      fresh.leaf.header = rel.leaves.front().leaf.header;
      fresh.leaf.header.page_uuid = r.new_pages[taken].page_uuid;
      fresh.leaf.header.page_number = fresh.page;
      fresh.leaf.header.page_generation = 1;
      fresh.leaf.body = rel.leaves.front().leaf.body;
      fresh.leaf.body.rows.clear();
      fresh.leaf.body.page_number = fresh.page;
      fresh.leaf.body.page_generation = 1;
      ++taken;
      target = &fresh;
    }
    if (!target) return std::nullopt;
    const auto final_metadata = finished(*target);
    auto into = target->leaf;
    pg::RowDataRecord row;
    row.row_uuid = {UuidKind::row, want.row_uuid};
    row.version_uuid = want.version_uuid;
    row.transaction_uuid = {UuidKind::transaction, r.transaction_uuid};
    row.local_transaction_id = r.transaction_order;
    row.row_version = 1;
    row.storage_generation = into.header.page_generation;
    u32 slot = 0;
    for (const auto& x : into.body.rows) slot = std::max(slot, x.stable_slot_id);
    row.stable_slot_id = slot + 1;
    const auto encoded = c::EncodeCatalogMetadataVersion(final_metadata);
    Check(encoded.ok(), "the row metadata encodes");
    scratchbird::core::datatypes::DatatypeBinaryValue cell;
    cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
    cell.payload = encoded.bytes;
    row.cells = {{1, std::move(cell), {}}};
    into.body.rows.push_back(std::move(row));
    const auto image = db::EncodeNativeCatalogLeaf(into);
    Check(image.ok(), "the expected leaf encodes");
    put(target->page, image.bytes);
    // SB_NATIVE_NAME_TREES: a name row's cells in the two name trees page zero
    // names (none when the file has no trees), each at its key's position in
    // the leaf that holds the key: the authoritative tree per entry, the
    // UUID-to-name tree per name row, the entry's payload carrying its path's
    // display components resolved over the file's names and the request's.
    if (want.name) {
      const auto zero = d::DecodeFilespacePageZero(file.data(), b.size);
      Check(zero.ok(), "page zero decodes");
      const auto root_of = [&](u16 kind) -> std::optional<u64> {
        for (const auto& root : zero.record->roots) if (root.kind == kind) return root.page_number;
        return std::nullopt;
      };
      const auto first = root_of(22), second = root_of(23);
      Check(first.has_value() == second.has_value(), "both name tree roots or neither");
      if (first) {
        std::deque<c::CatalogNamePayload> keep;
        c::CatalogNameIndex names;
        {
          RoleScope scope(b, 2);
          const auto name_relation = ReadRelation(b, file);
          for (const auto& l : name_relation.leaves)
            for (const auto& row : l.leaf.body.rows) {
              const auto& m = l.metadata.at(row.version_uuid);
              const c::CatalogNameVersionBinding at{
                  {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
                  row.row_uuid, {UuidKind::row, row.version_uuid}, m.record.header.object_uuid, row.transaction_uuid, l.page,
                  row.stable_slot_id, row.storage_generation, row.row_version, row.local_transaction_id, m.catalog_generation};
              const std::vector<byte> bytes(reinterpret_cast<const byte*>(m.record.payload.data()),
                                            reinterpret_cast<const byte*>(m.record.payload.data()) + m.record.payload.size());
              const auto decoded = c::DecodeCatalogNameEnvelope(bytes, at);
              Check(decoded.ok(), "a stored name decodes for the oracle");
              names.Add(c::BorrowCatalogNamePayload(keep.emplace_back(decoded.record->payload)));
            }
        }
        for (const auto& other : r.rows) if (other.name) names.Add(c::BorrowCatalogNamePayload(*other.name));
        const auto view = c::BorrowCatalogNamePayload(*want.name);
        struct Planned { unsigned tree; std::vector<byte> key, payload; };
        std::vector<Planned> cells;
        if (const auto* v = std::get_if<c::CatalogNameVectorView>(&view)) {
          const auto kind = std::optional{v->object_kind};
          Check(c::IsKnownCatalogRecordKind(*kind), "the vector's object kind is a record kind");
          cells.push_back({1, c::CatalogNameUuidTreeVectorKey(v->object_uuid.value, *kind), c::CatalogNameUuidTreeVectorPayload(*v)});
        } else {
          const auto& e = std::get<c::CatalogNameEntryView>(view);
          const auto kind = std::optional{e.object_kind};
          Check(c::IsKnownCatalogRecordKind(*kind), "the entry's object kind is a record kind");
          const auto path = c::ResolveCatalogNamePath(e, names);
          Check(path.ok(), "the entry's path resolves for the oracle");
          std::vector<std::string_view> components;
          for (const auto& component : path.components) components.push_back(component.display_name);
          cells.push_back({0, c::CatalogNameAuthoritativeTreeKey(e, *kind), c::CatalogNameAuthoritativeTreePayload(e)});
          cells.push_back({1, c::CatalogNameUuidTreeEntryKey(e, *kind), c::CatalogNameUuidTreeEntryPayload(e.name_entry_uuid.value, components)});
        }
        for (auto& planned : cells) {
          pg::NativeBtreeCell entry;
          entry.key.encoded_key = std::move(planned.key);
          entry.key.row_uuid = want.row_uuid;
          entry.key.version_uuid = want.version_uuid;
          entry.base_page = d::NativePageReference{b.primary, target->page, into.header.page_generation, b.profile};
          entry.creator_local_transaction_id = r.transaction_order;
          entry.payload = std::move(planned.payload);
          if (!OracleInsertCell(b, file, planned.tree ? *second : *first, std::move(entry), new_pages, r.new_pages, taken))
            return std::nullopt;
        }
      }
    }
    if (!rel.indexed) continue;
    pg::NativeBtreeCell entry;
    entry.key = KeyOf(want.row_uuid, want.version_uuid);
    entry.base_page = d::NativePageReference{b.primary, target->page, into.header.page_generation, b.profile};
    entry.creator_local_transaction_id = r.transaction_order;  // SB_NATIVE_BTREE_COMPRESSED_PAGE
    const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
    Check(root.ok(), "the catalog root decodes");
    const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(), [&](const auto& h) { return h.role == b.role; });
    Check(head != root.root->roots.end(), "the relation's head");
    if (!OracleInsertCell(b, file, head->page.page_number, std::move(entry), new_pages, r.new_pages, taken)) return std::nullopt;
  }
  if (taken != new_pages.size()) return std::nullopt;  // the writer added a page the oracle did not need
  return file;
}
// SB_CATALOG_NEW_PAGES (W3b-3a): the writer may add base leaves from the
// allocation map (page-zero root kind 3).
db::CatalogVersionWriterBinding GrowingBinding(const Db& b) {
  auto binding = b.Binding();
  binding.allocation_root = b.map_root;
  return binding;
}
// The allocation record of `page` and whether it is allocated, through the
// production fixed-root lease on a copy of `file`; also whether every page
// below it is allocated (the allocator takes the lowest free slot).
struct Allocation {
  std::optional<pg::NativeAllocationRecord> record;
  bool lower_all_allocated = false;
};
Allocation AllocationOf(const Db& b, std::span<const byte> file, u64 page) {
  const auto path = b.Copy(file, "allocation.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "allocation read-only open");
  Allocation out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    Check(fixed.ok(), "the fixed-root lease admits the file");
    if (const auto* r = db::detail::NativeFixedRootRecord(fixed.lease->allocation(), page)) out.record = *r;
    out.lower_all_allocated = true;
    for (u64 n = 0; n < page; ++n)
      if (!db::detail::NativeFixedRootRecord(fixed.lease->allocation(), n)) out.lower_all_allocated = false;
  }
  Check(device.Close().ok(), "close");
  fs::remove(path);
  return out;
}
// An insert through a binding that may add pages, checked against the oracle
// given the pages the writer allocated; each allocation's record is checked on
// its own and the map pages are then taken from the file.
std::vector<u64> ExpectGrowingInsert(const Db& b, d::CarefulWriteSink& sink, const std::string& path,
                                     const db::CatalogInsertRequest& r, std::string_view label) {
  const auto before = ReadAll(path);
  const auto result = db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
  if (!result.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.ok(), std::string(label) + ": the rows are inserted");
  const auto after = ReadAll(path);
  auto expected = InsertOracle(b, before, r, result.allocated_pages);
  Check(expected.has_value(), std::string(label) + ": the oracle places the rows on the pages the writer added");
  for (std::size_t i = 0; i < result.allocated_pages.size(); ++i) {
    const auto page = result.allocated_pages[i];
    const auto was = AllocationOf(b, before, page);
    const auto now = AllocationOf(b, after, page);
    // A base leaf (owned by its relation) or a split's tree leaf (owned by its index).
    const auto header = d::DecodeNativeCommonPageHeader(after.data() + page * b.size, 128);
    Check(!was.record && header.ok(), std::string(label) + ": a free slot, now a page");
    Uuid owner{};
    if (header.header->page_type == 6) {
      const auto leaf = db::DecodeNativeCatalogLeaf(PageOf(after, page, b.size));
      Check(leaf.ok(), std::string(label) + ": the new base leaf decodes");
      owner = leaf.page->body.relation_uuid.value;
    } else {
      const auto tree_page = pg::DecodeNativeBtreePage(PageOf(after, page, b.size));
      Check(tree_page.ok() && (header.header->page_type == 0x202 || header.header->page_type == 0x201) &&
                tree_page.page->creator_transaction_uuid.is_nil() && !tree_page.page->creator_local_transaction_id,
            std::string(label) + ": the new tree page (a leaf or a branch) decodes, the system's");
      owner = tree_page.page->dependencies.index_uuid;
    }
    Check(now.record && now.record->page_type == header.header->page_type && now.record->owner_uuid == owner &&
              now.record->page_uuid == r.new_pages[i].page_uuid && now.record->allocation_uuid == r.new_pages[i].allocation_uuid &&
              now.record->creator_transaction_uuid.is_nil() && !now.record->creator_local_transaction_id &&
              now.record->creator_operation_uuid.is_nil() && now.record->page_generation == 1,
          std::string(label) + ": the system's allocation record for the new leaf");
  }
  if (!result.allocated_pages.empty())
    Check(AllocationOf(b, before, result.allocated_pages.front()).lower_all_allocated,
          std::string(label) + ": the lowest free slot");
  for (u64 m = 1; m <= b.receipt.map_pages; ++m)
    std::copy(after.begin() + static_cast<std::ptrdiff_t>(m * b.size), after.begin() + static_cast<std::ptrdiff_t>((m + 1) * b.size),
              expected->begin() + static_cast<std::ptrdiff_t>(m * b.size));
  Check(after == *expected, std::string(label) + ": the file is the oracle's");
  return result.allocated_pages;
}

// A sink that records the offset of every write, in order.
class InsertRecordingSink final : public d::CarefulWriteSink {
 public:
  explicit InsertRecordingSink(d::CarefulWriteSink& inner) : inner_(inner) {}
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    offsets.push_back(offset);
    return inner_.WriteAt(offset, bytes);
  }
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  std::vector<u64> offsets;

 private:
  d::CarefulWriteSink& inner_;
};

db::CatalogVersionWriteResult Insert(const Db& b, d::CarefulWriteSink& sink, const db::CatalogInsertRequest& r,
                                     bool read_only = false) {
  auto binding = b.Binding();
  binding.read_only_database = read_only;
  return db::WriteCatalogInsertVersions(sink, binding, r);
}
// One insert through the writer, checked against the oracle; returns the
// writes' offsets in issue order.
std::vector<u64> ExpectInsert(const Db& b, Session& s, const std::string& path, const db::CatalogInsertRequest& r,
                              std::string_view label) {
  const auto before = ReadAll(path);
  const auto expected = InsertOracle(b, before, r);
  Check(expected.has_value(), std::string(label) + ": the oracle finds room");
  InsertRecordingSink recording(s.sink());
  const auto result = Insert(b, recording, r);
  if (!result.ok())
    std::cerr << label << ": " << db::CatalogVersionWriteErrorName(result.error)
              << (result.failed_row ? " row " + std::to_string(*result.failed_row) : "") << '\n';
  Check(result.ok(), std::string(label) + ": the rows are inserted");
  Check(ReadAll(path) == *expected, std::string(label) + ": the file is the oracle's");
  std::size_t changed = 0;
  for (u64 p = 0; p * b.size < before.size(); ++p)
    if (!std::equal(before.begin() + static_cast<std::ptrdiff_t>(p * b.size),
                    before.begin() + static_cast<std::ptrdiff_t>((p + 1) * b.size),
                    expected->begin() + static_cast<std::ptrdiff_t>(p * b.size)))
      ++changed;
  Check(result.writes == changed && recording.offsets.size() == changed, std::string(label) + ": one write per changed page");
  return recording.offsets;
}
// SB_CATALOG_NO_TEXT_IDENTIFIERS: rows changed size, so create packs its leaves
// differently, and a leaf a check inserts into may have no room left. Then the
// same insert takes new pages (two identities: a base leaf and a tree leaf), and
// is checked as a growing insert. `seed` keeps the identities distinct.
std::vector<u64> ExpectInsertOrGrow(const Db& b, Session& s, const std::string& path, db::CatalogInsertRequest r,
                                    std::string_view label, unsigned seed) {
  if (InsertOracle(b, ReadAll(path), r)) return ExpectInsert(b, s, path, r, label);
  // 3c: say which way it went, so a run shows each site that grew.
  std::cout << "insert or grow page_size=" << b.size << " grew: " << label << '\n';
  for (unsigned i = 0; i < 2; ++i)
    r.new_pages.push_back({V7(kT + 760000 + 4 * seed + i, 0xe70000 + seed), V7(kT + 760000 + 4 * seed + i, 0xe80000 + seed)});
  return ExpectGrowingInsert(b, s.sink(), path, r, std::string(label) + ", on a new page");
}
void InsertRefused(const Db& b, Session& s, const std::string& path, const db::CatalogInsertRequest& r, CE error,
                   std::string_view label, bool read_only = false) {
  const auto before = ReadAll(path);
  const auto result = Insert(b, s.sink(), r, read_only);
  if (result.error != error) std::cerr << label << ": got " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.error == error && result.writes == 0 && result.syncs == 0 && !result.attempted,
        std::string(label) + ": refused as expected with no write");
  Check(ReadAll(path) == before, std::string(label) + ": the file is unchanged");
}

// ---- Choosing where to insert ----------------------------------------------------------
// An indexed relation of the production database whose last index leaf and some
// reached base leaf both have room for a fixture row: (role, the largest row
// UUID of the relation). Measured capacity (W3b plan): at least one at every
// page size.
struct Place {
  u16 role = 0;
  Uuid largest;
};
std::vector<Place> Places(const Db& b, std::span<const byte> file) {
  std::vector<Place> out;
  // Not the names relation (role 2): it holds only name rows (W3b-1b-1).
  for (const u16 role : {u16{6}, u16{4}, u16{1}, u16{5}}) {
    RoleScope scope(b, role);
    const auto rel = ReadRelation(b, file);
    if (!rel.indexed) continue;
    const auto tree = ReadTree(b, file);
    const auto& last = tree.pages[tree.leaves.back()];
    if (last.page.cells.empty() || FreeCells(b, file, last.number) < 2) continue;
    Uuid largest = last.page.cells.back().key.row_uuid;
    const auto need = RowBytes(FixtureRow(b, file, role, V7(Ms(largest) + 1, 0x7fffff), V7(Ms(largest) + 1, 1), 2));
    u64 room = 0;
    for (const auto& l : rel.leaves) room = std::max<u64>(room, l.leaf.body.free_space_bytes);
    if (room < 2 * need) continue;
    out.push_back({role, largest});
  }
  return out;
}

// ---- Visibility by creator state ------------------------------------------------------
void States(const Db& b, const Place& at) {
  auto path = b.Copy(b.bytes, "states.db");
  RoleScope scope(b, at.role);
  u64 n = 2;
  const auto row_a = V7(Ms(at.largest) + 1, 0x10), row_b = V7(Ms(at.largest) + 2, 0x11),
             row_c = V7(Ms(at.largest) + 3, 0x12);
  {
    Session s(b, path);
    Begin(s, n);
    const auto file = ReadAll(path);
    const auto r = Request(n, {Row(b, file, at.role, row_a, 1, n)});
    const auto writes = ExpectInsert(b, s, path, r, "insert by a transaction in progress");
    Check(writes.size() == 2 && writes[0] != writes[1], "a base leaf, then an index leaf");
    const auto seen = Peek(b, path, row_a);
    Check(seen.read && !seen.version, "an insert in progress is not visible to a committed read");
    Commit(*s.w, s.sink(), n);
    PeekSees(b, path, row_a, VersionId(1), "after COMMIT");
  }
  ++n;
  {
    Session s(b, path);
    Begin(s, n);
    ExpectInsert(b, s, path, Request(n, {Row(b, ReadAll(path), at.role, row_b, 2, n)}), "an insert then rolled back");
    Rollback(*s.w, s.sink(), n);
    const auto seen = Peek(b, path, row_b);
    Check(seen.read && !seen.version, "a rolled-back insert is not visible; the relation still reads");
    // The row stays until sweep: its UUID cannot be inserted again.
    Begin(s, n + 1);
    InsertRefused(b, s, path, Request(n + 1, {Row(b, ReadAll(path), at.role, row_b, 3, n + 1)}), CE::duplicate_row,
                  "a rolled-back row's UUID");
    // A rolled-back row's object may be inserted again (the reader admits a
    // rolled-back duplicate).
    auto again = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 4, 0x13), 4, n + 1);
    const auto rel = ReadRelation(b, ReadAll(path));
    again.metadata.record.header.object_uuid = rel.At(rel.Find(VersionId(2)).first).metadata.at(VersionId(2)).record.header.object_uuid;
    ExpectInsertOrGrow(b, s, path, Request(n + 1, {again}), "a rolled-back row's object, inserted again", 1);
    Commit(*s.w, s.sink(), n + 1);
    PeekSees(b, path, again.row_uuid, VersionId(4), "the object inserted again, committed");
  }
  fs::remove(path);
  // A fresh copy: at 8 KiB the relation chosen holds four fixture rows.
  path = b.Copy(b.bytes, "states-dead.db");
  n = 2;
  {
    const auto lost = Row(b, ReadAll(path), at.role, row_c, 5, n);
    {
      Session s(b, path);
      Begin(s, n);
      ExpectInsert(b, s, path, Request(n, {lost}), "an insert then lost");
    }
    Session s(b, path);  // Restart: the transaction in progress is dead
    const auto seen = Peek(b, path, row_c);
    Check(seen.read && !seen.version, "an insert dead at restart is not visible");
    // A dead row's object may be inserted again (the reader passes over a
    // dead creator's row as over a rolled-back one).
    auto again = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 5, 0x14), 6, n + 1);
    again.metadata.record.header.object_uuid = lost.metadata.record.header.object_uuid;
    Begin(s, n + 1);
    ExpectInsert(b, s, path, Request(n + 1, {again}), "a dead row's object, inserted again");
    Commit(*s.w, s.sink(), n + 1);
    PeekSees(b, path, again.row_uuid, VersionId(6), "the dead row's object inserted again, committed");
  }
  fs::remove(path);
}

// ---- An object another transaction holds ------------------------------------------------
// MGA: only write-write blocks, and nothing waits. An object carried by a row
// another transaction in progress inserted refuses write_conflict naming it;
// once that transaction rolls back the object can be inserted; once it commits
// the object is a duplicate. One's own earlier insert is a duplicate.
void Conflicts(const Db& b, const Place& at) {
  const auto path = b.Copy(b.bytes, "conflicts.db");
  RoleScope scope(b, at.role);
  Session s(b, path);
  Begin(s, 2);
  Begin(s, 3);
  const auto x = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 1, 0x90), 90, 2);
  const auto object = x.metadata.record.header.object_uuid;
  ExpectInsert(b, s, path, Request(2, {x}), "an insert by transaction 2, in progress");
  const auto conflict = [&](u64 n, const db::CatalogInsertRow& row, u64 blocker, std::string_view label) {
    const auto before = ReadAll(path);
    const auto result = Insert(b, s.sink(), Request(n, {row}));
    if (result.error != CE::write_conflict) std::cerr << label << ": got " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.error == CE::write_conflict && result.blocker_order == blocker && result.failed_row == 0u &&
              result.writes == 0 && !result.attempted && ReadAll(path) == before,
          std::string(label) + ": write_conflict naming the holder, with no write");
  };
  auto y = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 2, 0x91), 91, 3);
  y.metadata.record.header.object_uuid = object;
  conflict(3, y, 2, "the object of transaction 2's insert in progress");
  auto own = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 3, 0x92), 92, 2);
  own.metadata.record.header.object_uuid = object;
  InsertRefused(b, s, path, Request(2, {own}), CE::duplicate_object, "an object this transaction already inserted");
  Rollback(*s.w, s.sink(), 2);
  y = Row(b, ReadAll(path), at.role, y.row_uuid, 91, 3);
  y.metadata.record.header.object_uuid = object;
  ExpectInsert(b, s, path, Request(3, {y}), "the object, once its holder rolled back");
  Begin(s, 4);
  auto z = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 4, 0x93), 93, 4);
  z.metadata.record.header.object_uuid = object;
  conflict(4, z, 3, "the object of transaction 3's insert in progress");
  Commit(*s.w, s.sink(), 3);
  InsertRefused(b, s, path, Request(4, {z}), CE::duplicate_object, "an object a committed transaction inserted");
  PeekSees(b, path, y.row_uuid, VersionId(91), "the object's committed row");
  fs::remove(path);
}

// ---- Keys inside and below the tree ---------------------------------------------------
void Keys(const Db& b, const std::vector<Place>& places) {
  const auto path = b.Copy(b.bytes, "keys.db");
  u64 n = 2;
  bool between = false, root_below = false, refused_below = false;
  for (const auto& at : places) {
    RoleScope scope(b, at.role);
    const auto file = ReadAll(path);
    const auto tree = ReadTree(b, file);
    // Between two keys of a leaf with room.
    for (const auto i : tree.leaves) {
      const auto& leaf = tree.pages[i];
      if (between || FreeCells(b, file, leaf.number) == 0 || leaf.page.cells.size() < 2) continue;
      for (std::size_t k = 0; k + 1 < leaf.page.cells.size() && !between; ++k) {
        const auto mid = Between(leaf.page.cells[k].key.row_uuid, leaf.page.cells[k + 1].key.row_uuid);
        if (!mid) continue;
        Session s(b, path);
        Begin(s, n);
        ExpectInsert(b, s, path, Request(n, {Row(b, ReadAll(path), at.role, *mid, 20, n)}), "a key between two keys");
        Commit(*s.w, s.sink(), n);
        PeekSees(b, path, *mid, VersionId(20), "the key between two keys, committed");
        ++n;
        between = true;
      }
    }
    // Below the smallest key.
    const auto& first = tree.pages[tree.leaves.front()];
    if (first.page.cells.empty()) continue;
    const auto below = V7(Ms(first.page.cells.front().key.row_uuid) - 1, 0x21);
    Session s(b, path);
    Begin(s, n);
    const auto r = Request(n, {Row(b, ReadAll(path), at.role, below, 21 + at.role, n)});
    if (FreeCells(b, ReadAll(path), first.number) == 0) {
      InsertRefused(b, s, path, r, CE::index_full, "a key below the smallest, its leaf full");
      refused_below = true;
    } else if (tree.root_is_leaf && !root_below) {
      ExpectInsert(b, s, path, r, "a key below the smallest, in a root leaf");
      Commit(*s.w, s.sink(), n);
      PeekSees(b, path, below, VersionId(21 + at.role), "the key below the smallest, committed");
      root_below = true;
    } else {
      Rollback(*s.w, s.sink(), n);
    }
    ++n;
  }
  Check(between, "a key between two keys was inserted");
  Check(root_below || refused_below, "a key below the smallest was tried");
  std::cout << b.name << " keys page_size=" << b.size << " between=" << between << " root_below=" << root_below
            << " index_full_below=" << refused_below << '\n';
  fs::remove(path);
}

// ---- A batch over two relations -------------------------------------------------------
void Batch(const Db& b, const std::vector<Place>& places) {
  if (places.size() < 2) {
    std::cout << b.name << " batch page_size=" << b.size << " skipped: one relation with room\n";
    return;
  }
  const auto path = b.Copy(b.bytes, "batch.db");
  const u64 n = 2;
  Session s(b, path);
  Begin(s, n);
  const auto file = ReadAll(path);
  const auto& p = places[0];
  const auto& q = places[1];
  const auto r = Request(n, {Row(b, file, p.role, V7(Ms(p.largest) + 1, 0x30), 30, n),
                             Row(b, file, q.role, V7(Ms(q.largest) + 1, 0x31), 31, n),
                             Row(b, file, p.role, V7(Ms(p.largest) + 2, 0x32), 32, n)});
  // Which pages are base leaves: every written page before the first index page.
  const auto writes = ExpectInsert(b, s, path, r, "a batch over two relations");
  const auto after = ReadAll(path);
  bool index_seen = false;
  for (const auto offset : writes) {
    const auto h = d::DecodeNativeCommonPageHeader(after.data() + offset, 128);
    Check(h.ok(), "a written page has a header");
    const bool index = h.header->page_type >= 0x200 && h.header->page_type <= 0x202;
    Check(index || !index_seen, "every base leaf is written before every index leaf");
    index_seen = index_seen || index;
  }
  Check(index_seen, "the batch wrote index leaves");
  Commit(*s.w, s.sink(), n);
  {
    RoleScope a(b, p.role);
    PeekSees(b, path, r.rows[0].row_uuid, VersionId(30), "batch row 1");
    PeekSees(b, path, r.rows[2].row_uuid, VersionId(32), "batch row 3");
  }
  {
    RoleScope a(b, q.role);
    PeekSees(b, path, r.rows[1].row_uuid, VersionId(31), "batch row 2");
  }
  fs::remove(path);
}

// ---- An unindexed relation --------------------------------------------------------------
void Unindexed(const Db& b) {
  const auto path = b.Copy(b.bytes, "unindexed.db");
  const u64 n = 2;
  RoleScope scope(b, 4);
  Session s(b, path);
  Begin(s, n);
  const auto row = V7(kT + 900000, 0x40);
  const auto writes = ExpectInsert(b, s, path, Request(n, {Row(b, ReadAll(path), 4, row, 40, n)}), "an unindexed insert");
  Check(writes.size() == 1, "one write: the head leaf");
  Commit(*s.w, s.sink(), n);
  PeekSees(b, path, row, VersionId(40), "the unindexed insert, committed");
  fs::remove(path);
}

// ---- Refusals before any write ----------------------------------------------------------
void InsertRefusals(const Db& b, const Place& at) {
  const auto path = b.Copy(b.bytes, "refusals.db");
  RoleScope scope(b, at.role);
  Session s(b, path);
  const u64 n = 2;
  Begin(s, n);
  const auto file = ReadAll(path);
  const auto fresh = V7(Ms(at.largest) + 1, 0x50);
  const auto good = Row(b, file, at.role, fresh, 50, n);
  const auto rel = ReadRelation(b, file);
  const auto& existing_leaf = rel.leaves.front();
  const auto& existing = existing_leaf.leaf.body.rows.front();
  const auto& existing_meta = existing_leaf.metadata.at(existing.version_uuid);
  {
    auto r = Request(n, {good});
    r.rows[0].row_uuid = existing.row_uuid.value;
    r.rows[0].metadata.record.header.row_uuid = existing.row_uuid;
    InsertRefused(b, s, path, r, CE::duplicate_row, "an existing row UUID");
  }
  {
    auto r = Request(n, {good});
    r.rows[0].version_uuid = existing.version_uuid;
    InsertRefused(b, s, path, r, CE::duplicate_row, "an existing version UUID");
  }
  {
    auto r = Request(n, {good});
    r.rows[0].metadata.record.header.object_uuid = existing_meta.record.header.object_uuid;
    InsertRefused(b, s, path, r, CE::duplicate_object, "a live row's object UUID");
  }
  {
    auto other = Row(b, file, at.role, V7(Ms(at.largest) + 2, 0x51), 51, n);
    other.metadata.record.header.object_uuid = good.metadata.record.header.object_uuid;
    InsertRefused(b, s, path, Request(n, {good, other}), CE::duplicate_object, "two rows of the batch, one object");
    other = good;
    other.version_uuid = VersionId(52);
    InsertRefused(b, s, path, Request(n, {good, other}), CE::duplicate_row, "two rows of the batch, one row");
  }
  const auto bad = [&](auto change, CE error, std::string_view label) {
    auto r = Request(n, {good});
    change(r.rows[0].metadata);
    InsertRefused(b, s, path, r, error, label);
  };
  bad([](auto& m) { m.definition_version = 2; }, CE::invalid_insert, "definition_version 2");
  bad([&](auto& m) { m.creator_transaction_uuid = {UuidKind::transaction, TxId(n + 1)}; }, CE::invalid_insert,
      "another transaction's creator");
  bad([&](auto& m) { m.creator_local_transaction_id = n + 1; }, CE::invalid_insert, "another transaction's number");
  bad([](auto& m) {
    m.record.header.deleted = true;
    m.lifecycle = c::CatalogObjectLifecycle::dropped;
    m.status = c::CatalogObjectStatus::retired;
  }, CE::invalid_insert, "a deleted first version");
  bad([](auto& m) { m.record.header.row_uuid.value = V7(1, 1); }, CE::invalid_insert, "metadata of another row");
  const auto root = RootOf(b, file);
  bad([&](auto& m) { m.catalog_generation = root.catalog_generation + 1; }, CE::ahead_of_catalog_root,
      "a generation above the root's");
  bad([&](auto& m) { m.security_epoch = root.security_epoch + 1; }, CE::ahead_of_catalog_root,
      "an epoch above the root's");
  // Name records are written since W3b-1b-1 (native_catalog_name_insert_test);
  // a name-kind row without its name is not one.
  bad([](auto& m) { m.record.header.kind = c::CatalogRecordKind::localized_name; }, CE::invalid_insert,
      "a name-kind row without its name");
  InsertRefused(b, s, path, Request(n, {}), CE::invalid_request, "no rows");
  {
    auto r = Request(n, {good});
    r.rows[0].role = 7;
    InsertRefused(b, s, path, r, CE::invalid_request, "role 7");
  }
  InsertRefused(b, s, path, Request(n, {good}), CE::read_only_database, "a read-only database", true);
  InsertRefused(b, s, path, Request(n + 1, {good}), CE::transaction_not_active, "a transaction never begun");
  Commit(*s.w, s.sink(), n);
  InsertRefused(b, s, path, Request(n, {good}), CE::transaction_not_active, "a committed transaction");
  Begin(s, n + 1, true);
  auto ro = Row(b, file, at.role, fresh, 53, n + 1);
  InsertRefused(b, s, path, Request(n + 1, {ro}), CE::read_only_transaction, "a read-only transaction");
  fs::remove(path);
}

// ---- Filling a relation's room ----------------------------------------------------------
void InsertFill(const Db& b, const Place& at) {
  const auto path = b.Copy(b.bytes, "fill.db");
  RoleScope scope(b, at.role);
  Session s(b, path);
  const u64 n = 2;
  Begin(s, n);
  unsigned k = 0;
  for (;; ++k) {
    Check(k < 4096, "the relation fills");
    const auto r = Request(n, {Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 1 + k, 0x60 + k), 600 + k, n)});
    if (!InsertOracle(b, ReadAll(path), r)) {
      const auto before = ReadAll(path);
      const auto result = Insert(b, s.sink(), r);
      Check((result.error == CE::relation_full || result.error == CE::index_full) && result.writes == 0 &&
                ReadAll(path) == before,
            "the insert past the room is refused with no write");
      std::cout << b.name << " fill page_size=" << b.size << " role=" << at.role << " inserted=" << k
                << " refused=" << db::CatalogVersionWriteErrorName(result.error) << '\n';
      break;
    }
    ExpectInsert(b, s, path, r, "an insert while there is room");
  }
  Check(k > 0, "at least one insert fit");
  fs::remove(path);
}

// ---- Process loss and torn writes ------------------------------------------------------
void InsertLosses(const Db& b, const Place& at) {
  RoleScope scope(b, at.role);
  const auto row = V7(Ms(at.largest) + 1, 0x70);
  for (u32 limit = 1; limit <= 2; ++limit)
    for (const bool torn : {false, true}) {
      const auto path = b.Copy(b.bytes, "loss.db");
      std::vector<byte> before;
      {
        Session s(b, path);
        Begin(s, 2);
        before = ReadAll(path);
      }
      const auto request = Request(2, {Row(b, before, at.role, row, 70, 2)});
      const auto expected = InsertOracle(b, before, request);
      Check(expected.has_value(), "the oracle places the lost insert");
      const pid_t child = fork();
      Check(child >= 0, "fork");
      if (child == 0) {
        d::FileDevice device;
        if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
        d::FileDeviceCarefulWriteSink file(device);
        if (torn) {
          scratchbird::tests::TornWriteSink sink(file, limit, kPlannedCrash);
          (void)Insert(b, sink, request);
        } else {
          scratchbird::tests::CrashAfterWritesSink sink(file, limit, kPlannedCrash);
          (void)Insert(b, sink, request);
        }
        _exit(kChildCompleted);
      }
      int status = 0;
      Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == kPlannedCrash,
            "the writer process was lost where planned");
      const auto after = ReadAll(path);
      // The pages the insert changes, in write order: the base leaf, then the index leaf.
      std::vector<u64> changed;
      for (u64 p = 0; p * b.size < before.size(); ++p)
        if (!std::equal(before.begin() + static_cast<std::ptrdiff_t>(p * b.size),
                        before.begin() + static_cast<std::ptrdiff_t>((p + 1) * b.size),
                        expected->begin() + static_cast<std::ptrdiff_t>(p * b.size)))
          changed.push_back(p);
      Check(changed.size() == 2, "the insert changes a base leaf and an index leaf");
      {
        // The base leaf is the page of type 6.
        const auto h = d::DecodeNativeCommonPageHeader(expected->data() + changed[0] * b.size, 128);
        if (h.ok() && h.header->page_type != 6) std::swap(changed[0], changed[1]);
      }
      for (std::size_t w = 0; w < changed.size(); ++w) {
        const auto offset = static_cast<std::ptrdiff_t>(changed[w] * b.size);
        const bool done = w + 1 < limit || (w + 1 == limit && !torn);
        const bool half = torn && w + 1 == limit;
        const auto& source = done ? *expected : before;
        if (half) {
          Check(std::equal(after.begin() + offset, after.begin() + offset + b.size / 2, expected->begin() + offset) &&
                    std::equal(after.begin() + offset + b.size / 2, after.begin() + offset + b.size,
                               before.begin() + offset + b.size / 2),
                "the torn write left its first half");
        } else {
          Check(std::equal(after.begin() + offset, after.begin() + offset + b.size, source.begin() + offset),
                "each page is its whole old or new image");
        }
      }
      for (u64 p = 0; p * b.size < before.size(); ++p)
        if (std::find(changed.begin(), changed.end(), p) == changed.end())
          Check(std::equal(after.begin() + static_cast<std::ptrdiff_t>(p * b.size),
                           after.begin() + static_cast<std::ptrdiff_t>((p + 1) * b.size),
                           before.begin() + static_cast<std::ptrdiff_t>(p * b.size)),
                "no other page changed");
      {
        Session restart(b, path);  // the lost transaction is dead
      }
      const auto seen = Look(b, path, row);
      // A tear at the half-way point leaves the whole new image when every byte
      // the write changes lies in the page's first half (an index page's seal is
      // at 512): that write is complete, not torn.
      bool really_torn = false;
      if (torn) {
        const auto offset = static_cast<std::ptrdiff_t>(changed[limit - 1] * b.size);
        really_torn = !std::equal(after.begin() + offset, after.begin() + offset + b.size, expected->begin() + offset);
      }
      if (really_torn) {
        Check(!seen.read, "a torn page fails its seal: the relation is refused until repaired (Q23 = 1)");
      } else {
        Check(seen.read && !seen.version, "a lost insert is not visible; the relation reads");
        // A new process inserts another row.
        Session s(b, path);
        const auto again = Request(3, {Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 2, 0x71), 71, 3)});
        Begin(s, 3);
        ExpectInsert(b, s, path, again, "an insert after the loss");
        Commit(*s.w, s.sink(), 3);
        PeekSees(b, path, again.rows[0].row_uuid, VersionId(71), "the insert after the loss, committed");
      }
      fs::remove(path);
    }
}

// ---- An index page torn at a sector boundary ---------------------------------------------
// The half-way tear above leaves an index page whole (its changes and seal lie
// in its first half). Here the index leaf's write keeps only the 512-byte
// sectors before the last one it changes: the page is neither its old nor its
// new image, fails its seal, and the relation is refused until repaired
// (Q23 = 1); the base leaf, written first, is whole.
class SectorTornSink final : public d::CarefulWriteSink {
 public:
  SectorTornSink(d::CarefulWriteSink& inner, u32 torn, std::size_t keep, int exit_code)
      : inner_(inner), torn_(torn), keep_(keep), exit_code_(exit_code) {}
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (++writes_ == torn_) {
      (void)inner_.WriteAt(offset, bytes.first(keep_));
      _exit(exit_code_);
    }
    return inner_.WriteAt(offset, bytes);
  }
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }

 private:
  d::CarefulWriteSink& inner_;
  u32 torn_;
  std::size_t keep_;
  int exit_code_;
  u32 writes_ = 0;
};
void TornIndex(const Db& b, const Place& at) {
  RoleScope scope(b, at.role);
  const auto row = V7(Ms(at.largest) + 1, 0x78);
  const auto path = b.Copy(b.bytes, "torn-index.db");
  std::vector<byte> before;
  {
    Session s(b, path);
    Begin(s, 2);
    before = ReadAll(path);
  }
  const auto request = Request(2, {Row(b, before, at.role, row, 78, 2)});
  const auto expected = InsertOracle(b, before, request);
  Check(expected.has_value(), "the oracle places the insert");
  std::optional<u64> index_page, base_page;
  for (u64 p = 0; p * b.size < before.size(); ++p) {
    const auto o = static_cast<std::ptrdiff_t>(p * b.size);
    if (std::equal(before.begin() + o, before.begin() + o + b.size, expected->begin() + o)) continue;
    const auto h = d::DecodeNativeCommonPageHeader(expected->data() + o, 128);
    Check(h.ok(), "a changed page has a header");
    (h.header->page_type == 6 ? base_page : index_page) = p;
  }
  Check(index_page && base_page, "the insert changes a base leaf and an index leaf");
  const auto o = static_cast<std::ptrdiff_t>(*index_page * b.size);
  std::size_t first = b.size, last = 0;
  for (std::size_t i = 0; i < b.size; ++i)
    if (before[o + i] != (*expected)[o + i]) {
      first = std::min(first, i);
      last = i;
    }
  const std::size_t keep = last / 512 * 512;
  Check(first < keep, "the index page's changes span more than one sector");
  const pid_t child = fork();
  Check(child >= 0, "fork");
  if (child == 0) {
    d::FileDevice device;
    if (!device.Open(path, d::FileOpenMode::open_existing).ok()) _exit(kChildFailed);
    d::FileDeviceCarefulWriteSink file(device);
    SectorTornSink sink(file, 2, keep, kPlannedCrash);
    (void)Insert(b, sink, request);
    _exit(kChildCompleted);
  }
  int status = 0;
  Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == kPlannedCrash,
        "the writer process was lost in the index leaf's write");
  const auto after = ReadAll(path);
  const auto ob = static_cast<std::ptrdiff_t>(*base_page * b.size);
  Check(std::equal(after.begin() + ob, after.begin() + ob + b.size, expected->begin() + ob), "the base leaf is whole");
  Check(std::equal(after.begin() + o, after.begin() + o + keep, expected->begin() + o) &&
            std::equal(after.begin() + o + keep, after.begin() + o + b.size, before.begin() + o + keep),
        "the index leaf holds its new sectors up to the tear and its old ones after");
  Check(!std::equal(after.begin() + o, after.begin() + o + b.size, expected->begin() + o) &&
            !std::equal(after.begin() + o, after.begin() + o + b.size, before.begin() + o),
        "the torn index leaf is neither its old nor its new image");
  Check(!pg::DecodeNativeBtreePage(PageOf(after, *index_page, b.size)).ok(), "the torn index leaf fails its seal");
  {
    Session restart(b, path);
  }
  Check(!Look(b, path, row).read, "a torn index leaf: the relation is refused until repaired (Q23 = 1)");
  std::cout << b.name << " torn index page_size=" << b.size << " changed=[" << first << "," << last << "] kept=" << keep
            << '\n';
  fs::remove(path);
}

// ---- I/O failures --------------------------------------------------------------------------
// Fails the k-th call of one kind (read, write or sync).
class KindFaultSink final : public d::CarefulWriteSink {
 public:
  enum class Kind { read, write, sync };
  KindFaultSink(d::CarefulWriteSink& inner, Kind kind, u32 k) : inner_(inner), kind_(kind), k_(k) {}
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    ++writes;
    if (kind_ == Kind::write && writes == k_) return Fail();
    return inner_.WriteAt(offset, bytes);
  }
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override {
    ++reads;
    if (kind_ == Kind::read && reads == k_) return Fail();
    return inner_.ReadAt(offset, bytes);
  }
  d::BoundedIoResult Sync() override {
    ++syncs;
    if (kind_ == Kind::sync && syncs == k_) return Fail();
    return inner_.Sync();
  }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  u32 reads = 0, writes = 0, syncs = 0;

 private:
  static d::BoundedIoResult Fail() {
    d::BoundedIoResult r;
    r.error = d::BoundedIoError::native_failure;
    r.native_error = EIO;
    r.native_attempted = true;
    return r;
  }
  d::CarefulWriteSink& inner_;
  Kind kind_;
  u32 k_;
};
void InsertFaults(const Db& b, const Place& at) {
  RoleScope scope(b, at.role);
  const auto row = V7(Ms(at.largest) + 1, 0x80);
  // Count each kind of call on a clean insert.
  u32 reads = 0, writes = 0, syncs = 0;
  {
    const auto path = b.Copy(b.bytes, "count.db");
    Session s(b, path);
    Begin(s, 2);
    KindFaultSink count(s.sink(), KindFaultSink::Kind::read, 0);
    Check(Insert(b, count, Request(2, {Row(b, ReadAll(path), at.role, row, 80, 2)})).ok(), "a clean insert");
    reads = count.reads;
    writes = count.writes;
    syncs = count.syncs;
    fs::remove(path);
  }
  Check(reads > 0 && writes == 2, "the insert reads, then writes two pages");
  unsigned points = 0, unattempted = 0;
  for (const auto kind : {KindFaultSink::Kind::read, KindFaultSink::Kind::write, KindFaultSink::Kind::sync}) {
    const u32 calls = kind == KindFaultSink::Kind::read ? reads : kind == KindFaultSink::Kind::write ? writes : syncs;
    for (u32 k = 1; k <= calls; ++k) {
      const auto path = b.Copy(b.bytes, "fault.db");
      Session s(b, path);
      Begin(s, 2);
      const auto before = ReadAll(path);
      const auto request = Request(2, {Row(b, before, at.role, row, 80, 2)});
      const auto expected = InsertOracle(b, before, request);
      KindFaultSink sink(s.sink(), kind, k);
      const auto result = Insert(b, sink, request);
      Check(!result.ok(), "the failed call fails the insert");
      const auto after = ReadAll(path);
      // A read fails before any write (the relation read, or the first page's
      // preimage): nothing is written and the insert then succeeds. A later
      // preimage read, a write or a sync fails after a write was attempted:
      // write_failed, and the file holds a prefix of the writes.
      if (!result.attempted) {
        Check(kind == KindFaultSink::Kind::read && result.writes == 0 && after == before,
              "a read that fails before any write writes nothing");
        Check(Insert(b, s.sink(), request).ok(), "the insert then succeeds");
        ++unattempted;
      } else {
        Check(result.error == CE::write_failed, "a failure after a write was attempted is write_failed");
        // Each page is its old or new image; the base leaf before the index leaf.
        bool index_new = false, base_old = false;
        for (u64 p = 0; p * b.size < before.size(); ++p) {
          const auto o = static_cast<std::ptrdiff_t>(p * b.size);
          const bool is_old = std::equal(after.begin() + o, after.begin() + o + b.size, before.begin() + o);
          const bool is_new = std::equal(after.begin() + o, after.begin() + o + b.size, expected->begin() + o);
          Check(is_old || is_new, "every page is whole");
          const auto h = d::DecodeNativeCommonPageHeader(expected->data() + o, 128);
          if (!is_old && h.ok() && h.header->page_type >= 0x200 && h.header->page_type <= 0x202) index_new = true;
          if (is_old && !is_new && h.ok() && h.header->page_type == 6) base_old = true;
        }
        Check(!(index_new && base_old), "the index leaf is never written before its base leaf");
      }
      ++points;
      fs::remove(path);
    }
  }
  Check(unattempted > 0 && unattempted < points, "failures both before and after the first write");
  std::cout << b.name << " insert faults page_size=" << b.size << " points=" << points << " before_any_write=" << unattempted
            << " reads=" << reads
            << " writes=" << writes << " syncs=" << syncs << '\n';
}
// The page type the file holds at `page`.
u32 TypeAt(const Db& b, std::span<const byte> file, u64 page) {
  const auto h = d::DecodeNativeCommonPageHeader(file.data() + page * b.size, 128);
  Check(h.ok(), "a page header decodes");
  return h.header->page_type;
}
// Every committed row's visible version, by row, from one committed read of
// a copy of the file through the production fixed-root lease and visibility
// reader; nullopt when the relation does not read.
std::optional<std::map<Uuid, Uuid>> PeekAll(const Db& b, const std::string& path) {
  const auto copy = b.Copy(ReadAll(path), "peek-all.db");
  d::FileDevice device;
  Check(device.Open(copy, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  std::optional<std::map<Uuid, Uuid>> out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    Check(fixed.ok(), "the fixed-root lease admits the file");
    const auto pages = [&](std::size_t bytes) { return bytes / b.size; };
    const db::NativeCatalogRelationMemoryLimits limits{b.profile, 1, pages(16u << 20), 8192, 8192,
                                                       {pages(4u << 20), 4u << 20}};
    auto prepared = db::PrepareNativeCatalogVisibilityRead(limits, g.memory, g.binding);
    Check(prepared.ok(), "visibility workspace");
    const auto read = db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*fixed.lease, 2, b.role, b.Relation(),
                                                                            2048 * u64{b.size}, prepared.workspace);
    if (read.ok()) {
      out.emplace();
      for (const auto& v : read.rows) (*out)[v.metadata.record.header.row_uuid.value] = v.version_uuid;
    }
  }
  Check(device.Close().ok(), "close the reader");
  fs::remove(copy);
  return out;
}
// Whether every page of the tree rooted at `root_page` that a sibling link
// names is also a child of a parent (no split is left unfinished).
bool TreeComplete(const Db& b, std::span<const byte> file, u64 root_page) {
  const auto tree = ReadTreeAt(b, file, root_page);
  std::set<u64> listed;
  for (const auto& p : tree.pages) listed.insert(p.number);
  for (const auto& p : tree.pages)
    if (p.page.right && !listed.contains(p.page.right->page_number)) return false;
  return true;
}
u64 HeadPage(const Db& b, std::span<const byte> file) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(), [&](const auto& h) { return h.role == b.role; });
  Check(head != root.root->roots.end(), "the relation's head");
  return head->page.page_number;
}
// SB_CATALOG_SPLIT_LAG (owner Q45 = 1): the insert that split, again on copies
// of the file before it, by a fresh transaction, failing each of its writes in
// turn (a crash at that point). Every time the relation still reads (a split
// left short of its parent is walked through the old leaf's right link); the
// failed transaction rolls back; the next insert, by another transaction,
// completes any unfinished split, commits and is read back, and then no tree
// page is reachable by a sibling link only.
void SplitCrashes(const Db& b, const Place& at, std::span<const byte> pre, const Uuid& row, unsigned k, bool root_split) {
  unsigned failures = 0, unfinished = 0;
  for (u32 w = 1; w <= 16; ++w) {
    const auto copy = b.Copy(pre, "split-crash.db");
    Session c(b, copy);  // restart: the earlier rows' transaction (2) was in progress, now dead
    Begin(c, 3);
    auto r = Request(3, {Row(b, ReadAll(copy), at.role, row, 9000 + k, 3)});
    for (unsigned i = 0; i < 4; ++i)
      r.new_pages.push_back({V7(kT + 810000 + 10 * w + i, 0xe50000 + w), V7(kT + 810000 + 10 * w + i, 0xe60000 + w)});
    KindFaultSink sink(c.sink(), KindFaultSink::Kind::write, w);
    const auto result = db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
    if (result.ok()) {
      fs::remove(copy);
      break;
    }
    ++failures;
    Rollback(*c.w, c.sink(), 3);
    const auto crashed = ReadAll(copy);
    if (!TreeComplete(b, crashed, HeadPage(b, crashed))) ++unfinished;
    const auto seen = Peek(b, copy, at.largest);
    Check(seen.read && seen.version.has_value(), "after a failed write " + std::to_string(w) + " the relation still reads");
    Begin(c, 4);
    auto next = Request(4, {Row(b, ReadAll(copy), at.role, V7(Ms(row) + 50, 0x910000 + w), 9500 + k, 4)});
    for (unsigned i = 0; i < 4; ++i)
      next.new_pages.push_back({V7(kT + 860000 + 10 * w + i, 0xeb0000 + w), V7(kT + 860000 + 10 * w + i, 0xec0000 + w)});
    const auto completed = db::WriteCatalogInsertVersions(c.sink(), GrowingBinding(b), next);
    if (!completed.ok()) std::cerr << "after failed write " << w << ": " << db::CatalogVersionWriteErrorName(completed.error) << '\n';
    Check(completed.ok(), "the next insert succeeds after failed write " + std::to_string(w));
    Commit(*c.w, c.sink(), 4);
    PeekSees(b, copy, next.rows.front().row_uuid, next.rows.front().version_uuid, "the next insert, committed");
    const auto after = ReadAll(copy);
    Check(TreeComplete(b, after, HeadPage(b, after)), "the next write completes an unfinished split");
    fs::remove(copy);
  }
  // A root-leaf split rewrites the root last, after its two new pages: it
  // has no interrupted state to leave.
  Check(failures > 0 && (root_split ? unfinished == 0 : unfinished > 0),
        root_split ? "a root-leaf split leaves nothing unfinished at any failure point"
                   : "some failure points leave a split unfinished, and they are survived");
  std::cout << b.name << " split crashes page_size=" << b.size << " failure points " << failures << ", " << unfinished
            << " leaving a split unfinished\n";
}
// SB_CATALOG_LEAF_SPLIT (W3b-3a-2): rows inserted one at a time, by one
// transaction, until the relation's row-UUID tree splits its rightmost leaf
// (UUIDv7 keys ascend into the rightmost leaf). Every
// insert is checked byte for byte against the oracle. Then committed, every
// inserted row is read back through the production fixed-root lease and
// visibility reader (the system page has no creator to wait for).
void IndexGrowthOn(const Db& b, const Place& at);
// Both shapes, where the relations with room have them: a tree with leaves
// under its root (a leaf split) and a one-page tree (a root-leaf split,
// SB_CATALOG_ROOT_LEAF_SPLIT).
void IndexGrowth(const Db& b, const std::vector<Place>& places) {
  for (const bool one_page : {false, true}) {
    const Place* chosen = nullptr;
    for (const auto& p : places) {
      RoleScope probe(b, p.role);
      if (ReadTree(b, b.bytes).root_is_leaf == one_page) {
        chosen = &p;
        break;
      }
    }
    if (chosen) IndexGrowthOn(b, *chosen);
    else
      std::cout << b.name << " index growth page_size=" << b.size << " no relation with room has a "
                << (one_page ? "one-page" : "two-level") << " tree\n";
  }
}
void IndexGrowthOn(const Db& b, const Place& at) {
  RoleScope scope(b, at.role);
  const auto path = b.Copy(b.bytes, "index-growth.db");
  Session s(b, path);
  Begin(s, 2);
  // W3b-2b: keys after every key of the relation as BEGIN left it (its event
  // row, newer than the create's rows, is in role 6).
  u64 largest_ms = Ms(at.largest);
  {
    const auto tree = ReadTree(b, ReadAll(path));
    for (const auto& p : tree.pages)
      if (!p.page.tree_level)
        for (const auto& cell : p.page.cells) largest_ms = std::max(largest_ms, Ms(cell.key.row_uuid));
  }
  std::vector<std::pair<Uuid, Uuid>> inserted;
  std::optional<u64> split;
  // At 64 and 128 KiB a one-page tree holds thousands of rows: fill it,
  // checked for success only, in batches until its rightmost leaf has fewer
  // than 64 free cells; the rows that reach the split are then checked one by
  // one, byte for byte (every row is, at the smaller sizes). Batch rows take
  // keys below the checked rows' keys.
  unsigned batches = 0;
  for (;; ++batches) {
    const auto file = ReadAll(path);
    const auto tree = ReadTree(b, file);
    // Only where checking every row would take too long: 64 and 128 KiB.
    if (b.size < 65536 || FreeCells(b, file, tree.pages[tree.leaves.back()].number) < 64 || batches >= 200) break;
    std::vector<db::CatalogInsertRow> rows;
    for (unsigned i = 0; i < 40; ++i) {
      const unsigned n = batches * 40 + i;
      rows.push_back(Row(b, file, at.role, V7(largest_ms + 10 + n, 0xa00000 + n), 20000 + n, 2));
    }
    auto r = Request(2, rows);
    for (unsigned i = 0; i < 8; ++i)
      r.new_pages.push_back({V7(kT + 870000 + 10 * batches + i, 0xed0000 + batches), V7(kT + 870000 + 10 * batches + i, 0xee0000 + batches)});
    const auto filled = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
    if (!filled.ok()) std::cerr << "fill batch " << batches << ": " << db::CatalogVersionWriteErrorName(filled.error) << '\n';
    Check(filled.ok(), "a fill batch is inserted");
    for (const auto& row : r.rows) inserted.push_back({row.row_uuid, row.version_uuid});
  }
  const u64 base_ms = largest_ms + 10 + 40 * (batches + 1);
  unsigned k = 1;
  for (; k <= 1500 && !split; ++k) {
    const auto file = ReadAll(path);
    auto r = Request(2, {Row(b, file, at.role, V7(base_ms + k, 0x900000 + k), 9000 + k, 2)});
    // Up to three pages one insert can need (a base leaf, then a root-leaf
    // split's two leaves), and one to spare.
    for (unsigned i = 0; i < 4; ++i)
      r.new_pages.push_back({V7(kT + 800000 + 10 * k + i, 0xe50000 + k), V7(kT + 800000 + 10 * k + i, 0xe60000 + k)});
    if (!InsertOracle(b, file, r)) {
      const auto trial = b.Copy(file, "index-growth-trial.db");
      Session t(b, trial, false);
      const auto tried = db::WriteCatalogInsertVersions(t.sink(), GrowingBinding(b), r);
      fs::remove(trial);
      if (!tried.ok()) {
        const auto tree = ReadTree(b, file);
        std::cout << b.name << " index growth page_size=" << b.size << " role " << at.role << " stops at row " << k << ": "
                  << db::CatalogVersionWriteErrorName(tried.error) << " (tree pages " << tree.pages.size()
                  << ", root is leaf " << tree.root_is_leaf << ")\n";
        break;
      }
    }
    const auto pre = ReadAll(path);
    const auto pages = ExpectGrowingInsert(b, s.sink(), path, r, "row " + std::to_string(k));
    inserted.push_back({r.rows.front().row_uuid, r.rows.front().version_uuid});
    const auto now = ReadAll(path);
    for (const auto page : pages)
      if (TypeAt(b, now, page) == 0x202) split = page;
    if (split) {
      const auto before_root = pg::DecodeNativeBtreePage(PageOf(pre, HeadPage(b, pre), b.size));
      SplitCrashes(b, at, pre, r.rows.front().row_uuid, k, before_root.ok() && before_root.page->tree_level == 0);
    }
  }
  Check(split.has_value(), "the row-UUID tree's rightmost leaf splits within the rows tried");
  {
    const auto now = ReadAll(path);
    const auto root = pg::DecodeNativeBtreePage(PageOf(now, HeadPage(b, now), b.size));
    Check(root.ok() && root.page->tree_level >= 1, "the root has leaves under it");
    const auto page = pg::DecodeNativeBtreePage(PageOf(now, *split, b.size));
    // The new rightmost leaf holds the new row's cell, last. (An old leaf the
    // create packed to its limit cannot take the high fence an append split
    // adds, so the rule's middle split point is used: the oracle agrees.)
    Check(page.ok() && !page.page->right && page.page->left && !page.page->cells.empty() &&
              page.page->cells.back().key.row_uuid == inserted.back().first,
          "the split's new leaf is the rightmost, holding the new row's cell last");
    std::cout << b.name << " index growth page_size=" << b.size << " new leaf holds " << page.page->cells.size() << " cells\n";
  }
  Commit(*s.w, s.sink(), 2);
  // One committed read sees every inserted row's version.
  const auto all = PeekAll(b, path);
  Check(all.has_value(), "the relation reads, committed, past a split");
  for (const auto& [row, version] : inserted) {
    const auto found = all->find(row);
    Check(found != all->end() && found->second == version, "an inserted row, committed, past a split");
  }
  fs::remove(path);
  std::cout << b.name << " index growth page_size=" << b.size << " role " << at.role << " split after " << inserted.size() << " rows\n";
}
}  // namespace

#ifndef SB_CATALOG_INSERT_WRITER_TEST_WITHOUT_MAIN
int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const auto production = ProductionDb(profile);
    const auto seeded = SeededDb(profile);
    const auto places = Places(*production, production->bytes);
    Check(!places.empty(), "an indexed relation with room (measured at every page size)");
    std::cout << "relations with room page_size=" << production->size << ":";
    for (const auto& p : places) std::cout << ' ' << p.role;
    std::cout << '\n';
    States(*production, places.front());
    Conflicts(*production, places.front());
    Keys(*production, places);
    Batch(*production, places);
    InsertRefusals(*production, places.front());
    InsertFill(*production, places.front());
    InsertLosses(*production, places.front());
    TornIndex(*production, places.front());
    InsertFaults(*production, places.front());
    IndexGrowth(*production, places);
    Unindexed(*seeded);
    std::cout << "PASS native catalog insert writer page_size=" << production->size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog insert writer profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
#endif  // SB_CATALOG_INSERT_WRITER_TEST_WITHOUT_MAIN
