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
  };
  std::vector<Page> pages;
  std::vector<std::size_t> leaves;  // level-0 pages in key order
  bool root_is_leaf = false;
};
TreeSeen ReadTree(const Db& b, std::span<const byte> file) {
  const auto root = pg::DecodeNativeCatalogRoot(PageOf(file, b.catalog_root.page_number, b.size));
  Check(root.ok(), "the catalog root decodes");
  const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                 [&](const auto& r) { return r.role == b.role; });
  Check(head != root.root->roots.end() && head->page_type == 0x200, "an indexed relation head");
  TreeSeen out;
  std::vector<u64> stack{head->page.page_number};
  while (!stack.empty()) {
    const auto n = stack.back();
    stack.pop_back();
    auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
    Check(page.ok(), "an index page decodes");
    if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
    for (const auto& cell : page.page->cells)
      if (cell.child) stack.push_back(cell.child->page_number);
    if (!page.page->tree_level) {
      out.leaves.push_back(out.pages.size());
      if (n == head->page.page_number) out.root_is_leaf = true;
    }
    out.pages.push_back({n, std::move(*page.page)});
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
std::size_t LeafFor(const TreeSeen& t, const pg::NativeBtreeKey& key) {
  for (const auto i : t.leaves) {
    const auto& p = t.pages[i].page;
    if (p.low_fence && KeyOrder(key, *p.low_fence) < 0) continue;
    if (p.high_fence && KeyOrder(key, *p.high_fence) >= 0) continue;
    return i;
  }
  throw std::runtime_error("no leaf holds the key");
}
// Free 16-byte-key cells on an index page: a slot, a 144-byte record, the key.
std::size_t FreeCells(const Db& b, std::span<const byte> file, u64 page) {
  const auto image = PageOf(file, page, b.size);
  const u32 used = image[140] | (image[141] << 8) | (image[142] << 16) | (u32{image[143]} << 24);
  return (b.size - used) / 164;
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
  m.audit_uuid = like->audit_uuid;
  m.security_policy_uuid = like->security_policy_uuid;
  m.owning_schema_uuid = like->owning_schema_uuid;
  m.default_name_uuid = {};
  m.name_vector_uuid = {};
  m.creator_local_transaction_id = n;
  m.object_subtype = "w3b_insert_mechanics_fixture";
  m.trace_search_key = "W3B-1A-INSERT-FIXTURE";
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
std::optional<std::vector<byte>> InsertOracle(const Db& b, std::span<const byte> before, const db::CatalogInsertRequest& r) {
  std::vector<byte> file(before.begin(), before.end());
  const auto put = [&](u64 page, std::span<const byte> image) {
    std::copy(image.begin(), image.end(), file.begin() + static_cast<std::ptrdiff_t>(page * b.size));
  };
  for (const auto& want : r.rows) {
    RoleScope scope(b, want.role);
    const auto rel = ReadRelation(b, file);
    const auto need = RowBytes(want.metadata);
    const LeafSeen* target = nullptr;
    for (const auto& l : rel.leaves)
      if (!target && l.leaf.body.free_space_bytes >= need) target = &l;
    if (!target) return std::nullopt;
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
    const auto encoded = c::EncodeCatalogMetadataVersion(want.metadata);
    Check(encoded.ok(), "the row metadata encodes");
    scratchbird::core::datatypes::DatatypeBinaryValue cell;
    cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
    cell.payload = encoded.bytes;
    row.cells = {{1, std::move(cell), {}}};
    into.body.rows.push_back(std::move(row));
    const auto image = db::EncodeNativeCatalogLeaf(into);
    Check(image.ok(), "the expected leaf encodes");
    put(target->page, image.bytes);
    if (!rel.indexed) continue;
    auto tree = ReadTree(b, file);
    const auto key = KeyOf(want.row_uuid, want.version_uuid);
    auto& leaf = tree.pages[LeafFor(tree, key)];
    std::size_t at = 0;
    while (at < leaf.page.cells.size() && KeyOrder(leaf.page.cells[at].key, key) < 0) ++at;
    pg::NativeBtreeCell entry;
    entry.key = key;
    entry.base_page = d::NativePageReference{b.primary, target->page, into.header.page_generation, b.profile};
    leaf.page.cells.insert(leaf.page.cells.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry));
    const auto index = pg::EncodeNativeBtreePage(leaf.page);
    if (!index.ok()) return std::nullopt;
    put(leaf.number, index.bytes);
  }
  return file;
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
  for (const u16 role : {u16{6}, u16{4}, u16{2}, u16{1}, u16{5}}) {
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
    Begin(*s.w, s.sink(), n);
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
    Begin(*s.w, s.sink(), n);
    ExpectInsert(b, s, path, Request(n, {Row(b, ReadAll(path), at.role, row_b, 2, n)}), "an insert then rolled back");
    Rollback(*s.w, s.sink(), n);
    const auto seen = Peek(b, path, row_b);
    Check(seen.read && !seen.version, "a rolled-back insert is not visible; the relation still reads");
    // The row stays until sweep: its UUID cannot be inserted again.
    Begin(*s.w, s.sink(), n + 1);
    InsertRefused(b, s, path, Request(n + 1, {Row(b, ReadAll(path), at.role, row_b, 3, n + 1)}), CE::duplicate_row,
                  "a rolled-back row's UUID");
    // A rolled-back row's object may be inserted again (the reader admits a
    // rolled-back duplicate).
    auto again = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 4, 0x13), 4, n + 1);
    const auto rel = ReadRelation(b, ReadAll(path));
    again.metadata.record.header.object_uuid = rel.At(rel.Find(VersionId(2)).first).metadata.at(VersionId(2)).record.header.object_uuid;
    ExpectInsert(b, s, path, Request(n + 1, {again}), "a rolled-back row's object, inserted again");
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
      Begin(*s.w, s.sink(), n);
      ExpectInsert(b, s, path, Request(n, {lost}), "an insert then lost");
    }
    Session s(b, path);  // Restart: the transaction in progress is dead
    const auto seen = Peek(b, path, row_c);
    Check(seen.read && !seen.version, "an insert dead at restart is not visible");
    // A dead row's object may be inserted again (the reader passes over a
    // dead creator's row as over a rolled-back one).
    auto again = Row(b, ReadAll(path), at.role, V7(Ms(at.largest) + 5, 0x14), 6, n + 1);
    again.metadata.record.header.object_uuid = lost.metadata.record.header.object_uuid;
    Begin(*s.w, s.sink(), n + 1);
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
  Begin(*s.w, s.sink(), 2);
  Begin(*s.w, s.sink(), 3);
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
  Begin(*s.w, s.sink(), 4);
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
        Begin(*s.w, s.sink(), n);
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
    Begin(*s.w, s.sink(), n);
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
  Begin(*s.w, s.sink(), n);
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
  Begin(*s.w, s.sink(), n);
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
  Begin(*s.w, s.sink(), n);
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
  bad([](auto& m) { m.record.header.kind = c::CatalogRecordKind::localized_name; }, CE::name_record_unsupported,
      "a name record (W3b-1b)");
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
  Begin(*s.w, s.sink(), n + 1, true);
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
  Begin(*s.w, s.sink(), n);
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
        Begin(*s.w, s.sink(), 2);
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
        Begin(*s.w, s.sink(), 3);
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
    Begin(*s.w, s.sink(), 2);
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
    Begin(*s.w, s.sink(), 2);
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
      Begin(*s.w, s.sink(), 2);
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
}  // namespace

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
    Unindexed(*seeded);
    std::cout << "PASS native catalog insert writer page_size=" << production->size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog insert writer profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
