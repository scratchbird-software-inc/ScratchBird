// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_COMMENT_INSERT (step 4, owner Q77 = 2, Q79 = 1,
// Q81 = 1, Q82 = 1; 2026-11-13). Comment rows written in place by transactions
// on databases written by the production create, one page size per run
// (--profile N), through the catalog insert and successor writers, and read
// back by the production startup reader and verifiers (Verify, from
// native_catalog_name_insert_test, included below with its databases,
// transactions and helpers):
//   - a fresh database: the comments relation (role 7) is empty and its
//     (target, language) tree is an empty root; full verification passes;
//   - a comment on the database object: in progress (another transaction's
//     insert of the same pair is write_conflict naming it), committed
//     (visible; full verification passes and counts it; its tree cell);
//   - one comment per (target, language): a second one is duplicate_comment
//     (committed, and within one request); a second language is admitted;
//   - wrong role both ways: a comment row in role 1, a policy row in role 7
//     (inserts and successors): invalid_role;
//   - an edit is a successor version (the base cell stamped, a new cell); a
//     successor that changes the language or the target is invalid_successor;
//   - a drop frees the pair: the same (target, language) inserts again;
//   - comment rows are data: no transaction here cites an event for them
//     (the writer would otherwise refuse unaudited_change);
//   - the careful-write order: every prefix of an insert's writes leaves a
//     database startup accepts (the uncommitted comment invisible); a torn
//     write is refused or is the next prefix; after a crash the pair is free
//     and the next insert of it succeeds;
//   - a comment on an object that does not exist: the writer accepts it (it
//     does not look targets up; the caller owes that), startup refuses it
//     (missing_target);
//   - forged files: a duplicate (target, language) refused by the comment
//     verifier (duplicate_comment); a non-comment row in role 7 refused
//     (invalid_role); a comment row of another payload schema cannot be
//     written at all: no metadata subtype admits it.
#define SB_CATALOG_NAME_INSERT_TEST_WITHOUT_MAIN
#include "native_catalog_name_insert_test.cpp"

#include "catalog_comment_record_codec.hpp"
#include "catalog_language.hpp"
#include "catalog_localized_record_codec.hpp"

namespace {
Uuid CommentId(unsigned k) { return V7(kT + 900000 + k, 0xc10000 + k); }
Uuid CommentRowId(unsigned k) { return V7(kT + 900000 + k, 0xc20000 + k); }
Uuid CommentVersionId(unsigned k) { return V7(kT + 900000 + k, 0xc30000 + k); }
std::vector<db::CatalogNewPageIdentity> CommentPages(unsigned k) {
  std::vector<db::CatalogNewPageIdentity> out;
  for (unsigned i = 0; i < 4; ++i)
    out.push_back({V7(kT + 950000 + 8 * k + i, 0xc40000 + k), V7(kT + 950000 + 8 * k + i, 0xc50000 + k)});
  return out;
}

// The database object's committed definition (role 1).
c::CatalogMetadataVersion DatabaseDefinition(const Db& b, std::span<const byte> file) {
  RoleScope scope(b, 1);
  for (const auto& l : ReadRelation(b, file).leaves)
    for (const auto& [version, m] : l.metadata)
      if (m.record.header.kind == c::CatalogRecordKind::database) return m;
  throw std::runtime_error("no database definition");
}
std::string CommentPayload(const Uuid& comment, const Uuid& target, const Uuid& language, const std::string& text) {
  const auto encoded = c::EncodeCatalogComment({comment, target, language, text});
  Check(encoded.ok(), "the comment encodes");
  return {reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size()};
}
// A first comment version by transaction n, its envelope from the database
// object's (owner, security policy) as the create's rows are.
db::CatalogInsertRow CommentRow(const c::CatalogMetadataVersion& database, u64 n, unsigned k, const Uuid& language,
                                const std::string& text, u16 role = 7) {
  auto m = database;
  const auto target = database.record.header.object_uuid.value;
  m.record.header.kind = c::CatalogRecordKind::localized_comment;
  m.record.header.object_uuid = {UuidKind::object, CommentId(k)};
  m.record.header.row_uuid = {UuidKind::row, CommentRowId(k)};
  m.record.header.parent_uuid = {UuidKind::object, target};
  m.record.header.deleted = false;
  m.object_subtype = c::CatalogObjectSubtype::localized_comment;
  m.default_name_uuid = {};
  m.name_vector_uuid = {};
  m.owning_schema_uuid = {};
  m.retired_transaction_uuid = {};
  m.definition_version = 1;
  m.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  m.creator_local_transaction_id = n;
  m.audit_uuid = EventRef(n);  // data rows cite an event (any), not one of their own transaction
  m.record.payload = CommentPayload(CommentId(k), target, language, text);
  return {role, CommentRowId(k), CommentVersionId(k), m, std::nullopt};
}
db::CatalogVersionWriteResult InsertGrowing(const Db& b, d::CarefulWriteSink& sink, u64 n,
                                            std::vector<db::CatalogInsertRow> rows, unsigned k) {
  auto r = Request(n, std::move(rows));
  r.new_pages = CommentPages(k);
  return db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
}
// The newest version of comment row `k` in the comments relation.
std::pair<Uuid, c::CatalogMetadataVersion> NewestComment(const Db& b, std::span<const byte> file, unsigned k) {
  RoleScope scope(b, 7);
  const auto rel = ReadRelation(b, file);
  const auto versions = rel.Versions(CommentRowId(k));
  Check(!versions.empty(), "the comment row is in the comments relation");
  const auto& row = rel.Row(versions.front());
  return {row.version_uuid, rel.At(versions.front().first).metadata.at(row.version_uuid)};
}
db::CatalogVersionRequest CommentSuccessor(const Db& b, std::span<const byte> file, unsigned k, u64 n, unsigned version_k,
                                           std::optional<std::string> text, bool drop = false) {
  const auto [predecessor, base] = NewestComment(b, file, k);
  db::CatalogVersionRequest r;
  r.role = 7;
  r.transaction_order = n;
  r.transaction_uuid = TxId(n);
  r.row_uuid = CommentRowId(k);
  r.predecessor_version_uuid = predecessor;
  r.version_uuid = CommentVersionId(version_k);
  r.metadata = base;
  r.metadata.definition_version += 1;
  r.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  r.metadata.creator_local_transaction_id = n;
  if (text) {
    const auto decoded = c::DecodeCatalogComment(base.record.payload);
    r.metadata.record.payload = CommentPayload(decoded.record->comment_uuid, decoded.record->target_object_uuid,
                                               decoded.record->language_uuid, *text);
  }
  if (drop) {
    r.metadata.record.header.deleted = true;
    r.metadata.lifecycle = c::CatalogObjectLifecycle::dropped;
    r.metadata.status = c::CatalogObjectStatus::retired;
    r.metadata.retired_transaction_uuid = {UuidKind::transaction, TxId(n)};
  }
  r.new_pages = CommentPages(100 + version_k);
  return r;
}

// The comment tree as stored: page zero's kind-24 root, walked; its cells.
struct CommentCell {
  std::vector<byte> key;
  Uuid row, version;
  u64 creator = 0, superseding = 0;
};
std::vector<CommentCell> CommentTreeCells(const Db& b, std::span<const byte> file) {
  const auto zero = d::DecodeFilespacePageZero(file.data(), b.size, nullptr);
  Check(zero.ok(), "page zero decodes");
  const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& r) { return r.kind == 24; });
  Check(root != zero.record->roots.end() && root->page_type == 0x200, "page zero carries the comment tree root (kind 24)");
  std::vector<CommentCell> out;
  std::vector<u64> stack{root->page_number};
  while (!stack.empty()) {
    const auto n = stack.back();
    stack.pop_back();
    const auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
    Check(page.ok(), "a comment tree page decodes");
    if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
    for (const auto& cell : page.page->cells) {
      if (cell.child) stack.push_back(cell.child->page_number);
      if (!page.page->tree_level)
        out.push_back({cell.key.encoded_key, cell.key.row_uuid, cell.key.version_uuid, cell.creator_local_transaction_id,
                       cell.superseding_local_transaction_id});
    }
  }
  return out;
}
std::vector<byte> Key(const Uuid& target, const Uuid& language) {
  const auto k = c::CatalogCommentTreeKey(target, language);
  return {k.begin(), k.end()};
}

void ExpectComments(const Db& b, const std::string& path, std::size_t comments, std::string_view label) {
  const auto v = Verify(b, path);
  if (!v.full.ok())
    std::cerr << label << " full: names=" << int(v.full.names.error) << " comments=" << int(v.full.comments.error)
              << " indexes=" << int(v.full.indexes.error) << " namespaces=" << int(v.full.namespaces.error)
              << " audit=" << int(v.full.audit.error) << " security=" << int(v.full.security.error)
              << " paths=" << int(v.full.paths.error) << " name_trees=" << int(v.full.name_trees.error) << '\n';
  Check(v.full.ok(), std::string(label) + ": the reopened database passes full startup verification");
  Check(v.full.comments.comments == comments,
        std::string(label) + ": the comment verifier counts " + std::to_string(comments) + " live comments");
  Check(v.full.indexes.indexed_comment_trees == 1, std::string(label) + ": the comment tree's tuple resolves");
  Check(v.allocation, std::string(label) + ": every captured page is allocated as the map records");
}

void Fresh(const Db& b) {
  {
    RoleScope scope(b, 7);
    const auto rel = ReadRelation(b, b.bytes);
    std::size_t rows = 0;
    for (const auto& l : rel.leaves) rows += l.leaf.body.rows.size();
    Check(rel.indexed && rows == 0, "fresh: the comments relation is indexed and empty");
  }
  Check(CommentTreeCells(b, b.bytes).empty(), "fresh: the comment tree is empty");
  ExpectComments(b, b.Copy(b.bytes, "fresh-comments.db"), 0, "fresh");
}

// The comment scenarios, in order on one file.
void Comments(const Db& b) {
  const auto path = b.Copy(b.bytes, "comments.db");
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto target = database.record.header.object_uuid.value;
  const auto en = c::kCatalogLanguage_en, fr = c::kCatalogLanguage_fr_CA, de = c::kCatalogLanguage_de_DE;
  Session s(b, path);
  // A comment on the database object, in progress.
  Begin(s, 2);
  {
    const auto r = InsertGrowing(b, s.sink(), 2, {CommentRow(database, 2, 1, en, "The orders database.")}, 1);
    if (!r.ok()) std::cerr << "insert: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
    Check(r.ok() && r.writes > 0, "a comment on the database object is inserted");
    const auto cells = CommentTreeCells(b, ReadAll(path));
    Check(cells.size() == 1 && cells[0].key == Key(target, en) && cells[0].row == CommentRowId(1) &&
              cells[0].version == CommentVersionId(1) && cells[0].creator == 2 && !cells[0].superseding,
          "its cell in the comment tree: target | language, its row and version, its creator");
  }
  Begin(s, 3);
  {
    const auto before = ReadAll(path);
    const auto r = InsertGrowing(b, s.sink(), 3, {CommentRow(database, 3, 2, en, "Another.")}, 2);
    Check(r.error == CE::write_conflict && r.blocker_order == 2 && r.writes == 0 && ReadAll(path) == before,
          "the same pair while another transaction's comment is in progress: write_conflict naming it, no write");
  }
  Rollback(*s.w, s.sink(), 3);
  Commit(*s.w, s.sink(), 2);
  ExpectComments(b, path, 1, "one committed comment");
  // One comment per (target, language).
  Begin(s, 4);
  InsertRefused(b, s, path, Request(4, {CommentRow(database, 4, 3, en, "A duplicate.")}), CE::duplicate_comment,
                "a second comment for the same pair");
  InsertRefused(b, s, path,
                Request(4, {CommentRow(database, 4, 3, fr, "Premier."), CommentRow(database, 4, 4, fr, "Second.")}),
                CE::duplicate_comment, "two comments for one pair in one request");
  {
    const auto r = InsertGrowing(b, s.sink(), 4, {CommentRow(database, 4, 4, fr, "La base des commandes.")}, 4);
    Check(r.ok(), "a second language for the same target is admitted");
  }
  Commit(*s.w, s.sink(), 4);
  ExpectComments(b, path, 2, "two languages");
  // Wrong role both ways.
  Begin(s, 5);
  InsertRefused(b, s, path, Request(5, {CommentRow(database, 5, 6, de, "Falsch.", 1)}), CE::invalid_role,
                "a comment row in role 1");
  {
    RoleScope scope(b, 5);
    const auto rel = ReadRelation(b, ReadAll(path));
    const auto& [version, m] = *rel.leaves.front().metadata.begin();
    db::CatalogInsertRow policy{7, CommentRowId(7), CommentVersionId(7), m, std::nullopt};
    policy.metadata.record.header.row_uuid = {UuidKind::row, CommentRowId(7)};
    InsertRefused(b, s, path, Request(5, {policy}), CE::invalid_role, "a non-comment row in role 7");
  }
  // An edit: a successor version; the identity is kept.
  {
    auto r = CommentSuccessor(b, ReadAll(path), 1, 5, 11, "The orders database (edited).");
    auto wrong_role = r;
    wrong_role.role = 1;
    Refused(b, s, path, wrong_role, CE::invalid_role, "a comment successor in role 1");
    auto language = r;
    language.metadata.record.payload = CommentPayload(CommentId(1), target, de, "Sprache.");
    Refused(b, s, path, language, CE::invalid_successor, "a successor that changes the language");
    auto retarget = r;
    retarget.metadata.record.payload = CommentPayload(CommentId(1), DatabaseDefinition(b, b.bytes).owner_uuid.value, en, "x");
    Refused(b, s, path, retarget, CE::invalid_successor, "a successor that changes the target");
    const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!written.ok()) std::cerr << "edit: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok(), "an edit is a successor version of the comment row");
    const auto cells = CommentTreeCells(b, ReadAll(path));
    const auto base = std::find_if(cells.begin(), cells.end(), [](const auto& c) { return c.version == CommentVersionId(1); });
    const auto next = std::find_if(cells.begin(), cells.end(), [](const auto& c) { return c.version == CommentVersionId(11); });
    Check(base != cells.end() && base->superseding == 5 && next != cells.end() && next->key == Key(target, en) &&
              next->creator == 5 && !next->superseding,
          "the base version's cell is stamped by the editor; the edit has its own cell");
  }
  Commit(*s.w, s.sink(), 5);
  ExpectComments(b, path, 2, "an edited comment");
  {
    const auto [version, m] = NewestComment(b, ReadAll(path), 1);
    const auto decoded = c::DecodeCatalogComment(m.record.payload);
    Check(version == CommentVersionId(11) && decoded.ok() && decoded.record->comment == "The orders database (edited).",
          "the newest version carries the edited text");
  }
  // A drop frees the pair.
  Begin(s, 6);
  {
    const auto r = CommentSuccessor(b, ReadAll(path), 1, 6, 12, std::nullopt, true);
    const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!written.ok()) std::cerr << "drop: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok(), "a drop is a successor version");
  }
  Commit(*s.w, s.sink(), 6);
  ExpectComments(b, path, 1, "a dropped comment");
  Begin(s, 7);
  Check(InsertGrowing(b, s.sink(), 7, {CommentRow(database, 7, 5, en, "The orders database, again.")}, 5).ok(),
        "after a drop, the same (target, language) inserts again");
  Commit(*s.w, s.sink(), 7);
  ExpectComments(b, path, 2, "a comment inserted again after a drop");
}

// ---- A maximum-length comment, edited --------------------------------------------------
// A comment of the maximum length fills a leaf of the smallest page size, so
// its edit (another maximum-length version) fits no reached leaf and goes on a
// new base leaf (SB_CATALOG_NEW_PAGES), reached by a cell for it in the
// relation's row-UUID tree, with its cell in the comment tree. (step4: this
// also keeps the successor-on-a-new-leaf path reached, which the names
// relation's new layout no longer happens to reach.)
void MaxLengthEdit(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto path = b.Copy(b.bytes, "comment-max.db");
  Session s(b, path);
  Begin(s, 2);
  Check(InsertGrowing(b, s.sink(), 2,
                      {CommentRow(database, 2, 61, c::kCatalogLanguage_de_DE,
                                  std::string(c::kCatalogCommentTextMaxBytes, 'a'))},
                      61)
            .ok(),
        "a maximum-length comment is inserted");
  Commit(*s.w, s.sink(), 2);
  Begin(s, 3);
  const auto r = CommentSuccessor(b, ReadAll(path), 61, 3, 62, std::string(c::kCatalogCommentTextMaxBytes, 'b'));
  const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
  if (!written.ok()) std::cerr << "max edit: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok(), "a maximum-length edit is written");
  if (b.size == 8192)
    Check(written.allocated_pages.size() == 1 && written.page_number == written.allocated_pages.front() &&
              written.forward_link_page != written.page_number,
          "at 8 KiB the edit goes on a new base leaf, its predecessor's page carrying the forward link");
  std::cout << "page_size=" << b.size << " maximum-length comment edit: "
            << (written.allocated_pages.empty() ? "in place" : "on a new base leaf") << '\n';
  Commit(*s.w, s.sink(), 3);
  ExpectComments(b, path, 1, "an edited maximum-length comment");
}

// ---- Growth -------------------------------------------------------------------------
// Comments on many objects (the definitions of role 1: the database, its
// filespace, schemas, datatypes and domains), one insert each, until the
// comments relation has more than one base leaf and the comment tree more than
// one page (at 8 KiB; larger pages hold them all on one): new leaves from the
// allocation map, the tree's root split, and startup verification over all.
void CommentGrowth(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  std::vector<Uuid> targets;
  {
    RoleScope scope(b, 1);
    for (const auto& l : ReadRelation(b, b.bytes).leaves)
      for (const auto& [version, m] : l.metadata)
        if (m.record.header.kind != c::CatalogRecordKind::localized_name && !m.record.header.deleted)
          targets.push_back(m.record.header.object_uuid.value);
  }
  std::sort(targets.begin(), targets.end());
  const std::size_t count = std::min<std::size_t>(targets.size(), 200);
  Check(count >= 150, "enough objects to comment on");
  const auto path = b.Copy(b.bytes, "comment-growth.db");
  {
    Session s(b, path);
    Begin(s, 2);
    for (std::size_t i = 0; i < count; ++i) {
      auto row = CommentRow(database, 2, 1000 + static_cast<unsigned>(i), c::kCatalogLanguage_en,
                            "Comment number " + std::to_string(i) + " on this object.");
      row.metadata.record.header.parent_uuid = {UuidKind::object, targets[i]};
      row.metadata.record.payload = CommentPayload(CommentId(1000 + static_cast<unsigned>(i)), targets[i],
                                                   c::kCatalogLanguage_en,
                                                   "Comment number " + std::to_string(i) + " on this object.");
      const auto r = InsertGrowing(b, s.sink(), 2, {row}, 1000 + static_cast<unsigned>(i));
      if (!r.ok()) std::cerr << "growth " << i << ": " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok(), "a comment on another object is inserted");
    }
    Commit(*s.w, s.sink(), 2);
  }
  const auto file = ReadAll(path);
  std::size_t leaves = 0;
  {
    RoleScope scope(b, 7);
    leaves = ReadRelation(b, file).leaves.size();
  }
  const auto cells = CommentTreeCells(b, file);
  Check(cells.size() == count, "one tree cell per comment");
  std::cout << "page_size=" << b.size << " comment growth: " << count << " comments on " << leaves
            << " base leaves" << '\n';
  if (b.size == 8192) Check(leaves > 1, "at 8 KiB the comments relation grew new base leaves");
  ExpectComments(b, path, count, "many comments");
}

// ---- The careful-write order -------------------------------------------------------
void CommentLosses(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto path = b.Copy(b.bytes, "comment-record.db");
  std::vector<byte> pre;
  Writes recorded;
  {
    Session s(b, path);
    Begin(s, 2);
    pre = ReadAll(path);  // BEGIN and its event written, the comment not yet
    RecordingSink rec(s.sink());
    Check(InsertGrowing(b, rec, 2, {CommentRow(database, 2, 21, c::kCatalogLanguage_en, "Recorded.")}, 21).ok(),
          "the recorded insert");
    recorded = rec.writes;
  }
  Check(recorded.size() >= 3, "an insert writes the base leaf, the index leaf and the comment tree");
  for (std::size_t k = 0; k <= recorded.size(); ++k) {
    const auto file = Apply(pre, recorded, k);
    const auto at = b.Copy(file, "comment-prefix.db");
    ExpectComments(b, at, 0, "a crash after " + std::to_string(k) + " of the insert's writes");
    if (k < recorded.size()) {
      // A torn write (its first half only): startup refuses the torn page
      // (its whole-page seal fails), or the database verifies with nothing
      // uncommitted visible. The latter happens when every byte the write
      // changes lies in the page's first half (the whole write, in effect), or
      // when the torn page is one nothing reaches yet: a new base leaf is
      // written before the index page that points to it (owner Q23 = 1), so a
      // tear there leaves a damaged page no read visits (wasted space, Q17 =
      // 1). Each write is labelled by its page type.
      const auto torn = Apply(pre, recorded, k, 1);
      const auto v = Verify(b, b.Copy(torn, "comment-torn.db"), true);
      Check(v.refused || (v.full.ok() && v.full.comments.comments == 0),
            "a torn write is refused at startup, or leaves a database that verifies with the comment invisible");
      const auto whole = Apply(pre, recorded, k + 1);
      const u64 page = recorded[k].first / b.size;
      std::cout << "page_size=" << b.size << " comment insert write " << k << " of " << recorded.size() << " (page "
                << page << " type 0x" << std::hex << TypeAt(b, whole, page) << std::dec << ", " << recorded[k].second.size()
                << " bytes) torn: "
                << (v.refused ? "refused" : torn == whole ? "same as the whole write" : "admitted, verifies") << '\n';
    }
    if (k + 1 == recorded.size()) {
      // The leaves are written and the comment tree is not: after a restart
      // the creator is dead, its pair is free, and the next insert of it
      // succeeds and commits.
      {
      Session s(b, at);
      const auto begun = s.w->Begin(s.sink(), [] {
        pg::TransactionHeaderRecord h;
        h.transaction_uuid = TxId(40);
        return h;
      }());
      Check(begun.ok(), "BEGIN after the restart");
      const u64 n = *begun.transaction_order;
      Audit(s, n, TxId(40));
      auto row = CommentRow(database, n, 22, c::kCatalogLanguage_en, "After the crash.");
      row.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(40)};
      row.metadata.audit_uuid = EventRef(n);
      auto r = Request(n, {row});
      r.transaction_uuid = TxId(40);
      r.new_pages = CommentPages(22);
      const auto written = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
      if (!written.ok()) std::cerr << "after crash: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
      Check(written.ok(), "after the crash the pair is free and the insert succeeds");
      Check(s.w->Commit(s.sink(), n, CommitTime(40)).ok(), "its COMMIT");
      }
      ExpectComments(b, at, 1, "the insert after the crash, committed");
    }
  }
}

// ---- A comment on an object that does not exist -------------------------------------
// The writer does not look the target up (as the name writer does not look up
// a name's object): that a target exists, and that its comments are dropped
// with it, are rules the engine-API caller owes (open item). Startup refuses a
// database holding such a comment: missing_target.
void MissingTarget(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto path = b.Copy(b.bytes, "comment-missing-target.db");
  const auto nowhere = V7(kT + 990000, 0xc90000);
  {
    Session s(b, path);
    Begin(s, 2);
    auto row = CommentRow(database, 2, 41, c::kCatalogLanguage_en, "On nothing.");
    row.metadata.record.header.parent_uuid = {UuidKind::object, nowhere};
    row.metadata.record.payload = CommentPayload(CommentId(41), nowhere, c::kCatalogLanguage_en, "On nothing.");
    Check(InsertGrowing(b, s.sink(), 2, {row}, 41).ok(), "the writer accepts a comment on a target it does not look up");
    Commit(*s.w, s.sink(), 2);
  }
  const auto v = Verify(b, path);
  Check(!v.full.ok() && v.full.comments.error == ia::NativeCatalogCommentVerificationError::missing_target &&
            v.full.comments.referenced_uuid == nowhere,
        "startup refuses a committed comment whose target does not exist: missing_target");
}

// ---- Forged files -------------------------------------------------------------------
// Rewrites comment row `k`'s newest version in its leaf (and, when `key` is
// given, its tree cell's key, the cells kept in key order; an empty key
// removes the cell), re-encoded and
// re-sealed with the production encoders: what a damaged or hostile file
// could hold.
std::vector<byte> Forge(const Db& b, std::vector<byte> file, unsigned k, const c::CatalogMetadataVersion& replacement,
                        std::optional<std::vector<byte>> key) {
  const auto [version, unused] = NewestComment(b, file, k);
  (void)unused;
  RoleScope scope(b, 7);
  const auto rel = ReadRelation(b, file);
  const auto at = rel.Find(version);
  auto leaf = rel.At(at.first).leaf;
  const auto encoded = c::EncodeCatalogMetadataVersion(replacement);
  Check(encoded.ok(), "the forged row version encodes");
  leaf.body.rows[at.second].cells.front().value.payload = encoded.bytes;
  const auto sealed = db::EncodeNativeCatalogLeaf(leaf);
  Check(sealed.ok(), "the forged leaf encodes");
  std::copy(sealed.bytes.begin(), sealed.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(at.first * b.size));
  if (key) {
    const auto zero = d::DecodeFilespacePageZero(file.data(), b.size, nullptr);
    const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& r) { return r.kind == 24; });
    auto page = pg::DecodeNativeBtreePage(PageOf(file, root->page_number, b.size));
    Check(page.ok() && page.page->tree_level == 0, "the comment tree is one root leaf here");
    for (auto& cell : page.page->cells)
      if (cell.key.version_uuid == version) cell.key.encoded_key = *key;
    std::erase_if(page.page->cells, [](const auto& cell) { return cell.key.encoded_key.empty(); });
    std::sort(page.page->cells.begin(), page.page->cells.end(),
              [](const auto& x, const auto& y) { return pg::CompareNativeBtreeKeys(x.key, y.key) < 0; });
    const auto bytes = pg::EncodeNativeBtreePage(*page.page);
    Check(bytes.ok(), "the forged tree page encodes");
    std::copy(bytes.bytes.begin(), bytes.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(root->page_number * b.size));
  }
  return file;
}
void Forged(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto target = database.record.header.object_uuid.value;
  const auto path = b.Copy(b.bytes, "comment-forge.db");
  {
    Session s(b, path);
    Begin(s, 2);
    Check(InsertGrowing(b, s.sink(), 2,
                        {CommentRow(database, 2, 31, c::kCatalogLanguage_en, "English."),
                         CommentRow(database, 2, 32, c::kCatalogLanguage_fr_CA, "Fran\xc3\xa7" "ais.")},
                        31)
              .ok(),
          "two comments for the forgeries");
    Commit(*s.w, s.sink(), 2);
  }
  ExpectComments(b, path, 2, "before forging");
  const auto file = ReadAll(path);
  {
    // A duplicate: the French comment re-labelled English, row and cell.
    auto m = NewestComment(b, file, 32).second;
    m.record.payload = CommentPayload(CommentId(32), target, c::kCatalogLanguage_en, "Fran\xc3\xa7" "ais.");
    const auto forged = Forge(b, file, 32, m, Key(target, c::kCatalogLanguage_en));
    const auto v = Verify(b, b.Copy(forged, "forged-duplicate.db"));
    Check(!v.full.ok() && v.full.names.ok() &&
              v.full.comments.error == ia::NativeCatalogCommentVerificationError::duplicate_comment,
          "a forged duplicate (target, language) is refused by startup verification: duplicate_comment");
  }
  {
    // A non-comment row in role 7: a generic policy row (the insert writer
    // test's mechanics fixture) in the French comment's place, its tree cell
    // removed.
    const auto m = FixtureRow(b, file, 1, CommentId(32), CommentRowId(32), 2);
    const auto forged = Forge(b, file, 32, m, std::vector<byte>{});
    const auto v = Verify(b, b.Copy(forged, "forged-role.db"), true);
    if (v.refused) std::cerr << "forged role: capture refused error=" << int(v.refused->error) << '\n';
    Check(!v.refused && !v.full.ok() && v.full.names.error == ia::NativeCatalogNameVerificationError::invalid_role,
          "a non-comment row in the comments relation is refused by startup verification: invalid_role");
  }
  {
    // Another payload schema: the legacy 65542 record under kind
    // localized_comment, tried under every subtype.
    c::CatalogLocalizedCommentRecord legacy;
    legacy.target_object_uuid = {UuidKind::object, target};
    legacy.language = "fr-CA";
    legacy.comment = "legacy";
    legacy.creator_transaction_number = 2;
    const auto bytes = c::EncodeCatalogLocalizedComment(legacy);
    Check(bytes.ok(), "the legacy comment record encodes");
    auto m = NewestComment(b, file, 32).second;
    m.record.payload.assign(reinterpret_cast<const char*>(bytes.bytes.data()), bytes.bytes.size());
    m.object_subtype = c::CatalogObjectSubtype::localized_comment;
    Check(c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m)).has_value(),
          "the localized_comment subtype refuses the legacy payload: no encoder writes that row");
    std::optional<c::CatalogMetadataVersion> admitted;
    for (const auto& subtype : c::kCatalogSubtypeIdentities) {
      m.object_subtype = subtype.subtype;
      if (!c::ValidateCatalogMetadataVersionView(c::BorrowCatalogMetadataVersion(m))) {
        admitted = m;
        break;
      }
    }
    // SB_NATIVE_COMMENT_RECORD: a localized_comment row version is exactly
    // the 65678 record under subtype localized_comment; no subtype admits
    // another payload, so no production encoder writes such a row and the
    // leaf decoder (every startup read) refuses one.
    Check(!admitted, "no subtype admits a localized_comment row with another payload schema");
  }
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const auto production = ProductionDb(profile);
    const auto& b = *production;
    Fresh(b);
    Comments(b);
    MaxLengthEdit(b);
    CommentGrowth(b);
    CommentLosses(b);
    MissingTarget(b);
    Forged(b);
    std::cout << "PASS native catalog comment insert page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog comment insert profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
