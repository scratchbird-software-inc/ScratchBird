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
//     step4-open2 (owner 2026-11-13, Q86, Q87 = 1): the writer now looks the
//     target up and refuses such a comment (missing_target, nothing
//     written); startup's missing_target is still checked, on a forged file
//     whose committed comment is retargeted at nothing;
//   - step4-open2, SB_CATALOG_DEPENDENT_CASCADE / SB_NATIVE_COMMENT_TARGET
//     (DropCascade): a drop of a commented definition drops its comments (two
//     languages) in the same write, the file byte for byte the oracle's (the
//     version writer's successor oracle for the definition and for each
//     comment's drop version, the base cells stamped), every base leaf
//     written before any tree page; too few identities refused; committed and
//     reopened, startup verification passes and the comments are invisible;
//     every prefix of that write, and each write torn, decided consistently
//     at startup; a drop of an uncommented definition unchanged (the
//     successor oracle); another transaction's comment insert, edit or drop
//     in progress makes the drop a write_conflict, nothing written; another
//     transaction's drop of the target in progress makes a comment insert a
//     write_conflict; its ALTER in progress does not (Q87 = 1); this
//     transaction's own comment is dropped with its own drop; a comment on a
//     committed-dropped, a missing, or this transaction's own dropped target
//     is refused missing_target;
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
// step4-open2 (owner 2026-11-13, Q86, Q87 = 1; SB_NATIVE_COMMENT_TARGET): the
// writer now looks the target up. A comment on an object that does not exist
// is refused missing_target with nothing written (this scenario expected the
// writer to accept it, as the caller's rule; the expectation is changed
// because the writer now enforces the rule). Startup still refuses a database
// holding such a comment: a committed comment on the database object, forged
// (row payload, parent and tree key) to target nothing: missing_target.
std::vector<byte> Forge(const Db& b, std::vector<byte> file, unsigned k, const c::CatalogMetadataVersion& replacement,
                        std::optional<std::vector<byte>> key);
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
    const auto before = ReadAll(path);
    const auto r = InsertGrowing(b, s.sink(), 2, {row}, 41);
    if (r.error != CE::missing_target) std::cerr << "nowhere: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
    Check(r.error == CE::missing_target && r.failed_row == 0 && r.writes == 0 && !r.attempted && ReadAll(path) == before,
          "step4-open2: the writer refuses a comment on a target that does not exist: missing_target, nothing written");
    Check(InsertGrowing(b, s.sink(), 2, {CommentRow(database, 2, 42, c::kCatalogLanguage_en, "On the database.")}, 42).ok(),
          "a comment on the database object, to forge");
    Commit(*s.w, s.sink(), 2);
  }
  ExpectComments(b, path, 1, "before the forgery");
  auto m = NewestComment(b, ReadAll(path), 42).second;
  m.record.header.parent_uuid = {UuidKind::object, nowhere};
  m.record.payload = CommentPayload(CommentId(42), nowhere, c::kCatalogLanguage_en, "On the database.");
  const auto forged = Forge(b, ReadAll(path), 42, m, Key(nowhere, c::kCatalogLanguage_en));
  const auto v = Verify(b, b.Copy(forged, "forged-missing-target.db"));
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

// ---- Drops take their comments (step4-open2) ---------------------------------------
// SB_CATALOG_DEPENDENT_CASCADE, SB_NATIVE_COMMENT_TARGET (owner 2026-11-13:
// metadata behaves like ordinary rows under MGA; Q86: comments are fully
// dependent on their target; Q87 = 1: only a drop of the target conflicts with
// a comment change). Named objects in the definition relation the name tests
// use (startup requires a live name to name a live definition, so a drop
// retires the entry, the vector and the definition together, as
// NameSuccessors does).
std::vector<db::CatalogNewPageIdentity> DropPages(unsigned k) {
  std::vector<db::CatalogNewPageIdentity> out;
  for (unsigned i = 0; i < 6; ++i)
    out.push_back({V7(kT + 3100000 + 8 * k + i, 0xd10000 + k), V7(kT + 3100000 + 8 * k + i, 0xd20000 + k)});
  return out;
}
Uuid DependentId(unsigned k) { return V7(kT + 3200000 + k, 0xd30000 + k); }
// A first comment version on `target` by transaction n.
db::CatalogInsertRow CommentOn(const c::CatalogMetadataVersion& database, u64 n, unsigned k, const Uuid& target,
                               const Uuid& language, const std::string& text) {
  auto row = CommentRow(database, n, k, language, text);
  row.metadata.record.header.parent_uuid = {UuidKind::object, target};
  row.metadata.record.payload = CommentPayload(CommentId(k), target, language, text);
  return row;
}
db::CatalogVersionWriteResult InsertWithPages(const Db& b, d::CarefulWriteSink& sink, u64 n,
                                              std::vector<db::CatalogInsertRow> rows, unsigned k) {
  auto r = Request(n, std::move(rows));
  r.new_pages = DropPages(k);
  return db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
}
Uuid DefinitionObject(const Named& o) { return o.definition.metadata.record.header.object_uuid.value; }
// The drop of `o`'s definition by n (its newest version `predecessor`), with
// `dependents` drop-version identities.
db::CatalogVersionRequest DefinitionDrop(const Db& b, std::span<const byte> file, u16 role, const Named& o, u64 n,
                                         unsigned version_k, std::size_t dependents, bool drop = true) {
  RoleScope scope(b, role);
  const auto rel = ReadRelation(b, file);
  const auto versions = rel.Versions(o.definition.row_uuid);
  Check(!versions.empty(), "the definition row is in its relation");
  auto r = Successor(b, rel, o.definition.row_uuid, rel.Row(versions.front()).version_uuid, n, VersionId(version_k), drop);
  for (std::size_t i = 0; i < dependents; ++i) r.dependent_versions.push_back(DependentId(version_k * 10 + static_cast<unsigned>(i)));
  r.new_pages = DropPages(version_k);
  return r;
}
// The entry and the vector of `o` dropped by n (names are data: no event).
void DropNames(const Db& b, Session& s, const Named& o, u64 n, unsigned version_k) {
  auto e = std::get<c::CatalogNameEntry>(*o.entry.name);
  e.lifecycle_state = c::CatalogNameLifecycle::dropped;
  e.dropped_transaction_uuid = scratchbird::core::platform::TypedUuid{UuidKind::transaction, TxId(n)};
  auto entry_drop = NameSuccessor(o.entry, o.entry.version_uuid, n, VersionId(version_k), c::CatalogNamePayload{e}, true);
  entry_drop.metadata.record.payload.clear();
  const auto entry_written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), entry_drop);
  if (!entry_written.ok()) std::cerr << "entry drop: " << db::CatalogVersionWriteErrorName(entry_written.error) << '\n';
  Check(entry_written.ok(), "the entry dropped");
  auto v = std::get<c::CatalogNameVector>(*o.vector.name);
  v.lifecycle_state = c::CatalogNameLifecycle::dropped;
  const auto vector_written = db::WriteCatalogSuccessorVersion(
      s.sink(), GrowingBinding(b), NameSuccessor(o.vector, o.vector.version_uuid, n, VersionId(version_k + 1), c::CatalogNamePayload{v}, true));
  if (!vector_written.ok()) std::cerr << "vector drop: " << db::CatalogVersionWriteErrorName(vector_written.error) << '\n';
  Check(vector_written.ok(), "the vector dropped");
}
// The oracle for a drop that takes comments: the version writer's successor
// oracle (Oracle) for the definition's drop version in its relation, then, for
// each comment (row-UUID order) its drop version as the test builds it (the
// base's metadata by n, the next definition version, generation the larger,
// dropped and retired by n, the payload unchanged) placed by the same oracle
// in the comments relation, then the comment tree's cell of each base version
// stamped with n. Every page re-encoded by the production encoders.
std::optional<std::vector<byte>> CascadeOracle(const Db& b, std::span<const byte> before, u16 role,
                                               const db::CatalogVersionRequest& r,
                                               const std::vector<std::pair<unsigned, Uuid>>& comments) {
  std::vector<byte> file(before.begin(), before.end());
  {
    RoleScope scope(b, role);
    auto plain = r;
    plain.dependent_versions.clear();
    const auto e = Oracle(b, file, plain);
    if (!e) return std::nullopt;
    file = e->file;
  }
  std::set<Uuid> stamped;
  for (const auto& [k, version] : comments) {
    const auto [predecessor, base] = NewestComment(b, file, k);
    db::CatalogVersionRequest cr;
    cr.role = 7;
    cr.transaction_order = r.transaction_order;
    cr.transaction_uuid = r.transaction_uuid;
    cr.row_uuid = CommentRowId(k);
    cr.predecessor_version_uuid = predecessor;
    cr.version_uuid = version;
    cr.metadata = base;
    cr.metadata.definition_version += 1;
    cr.metadata.creator_transaction_uuid = {UuidKind::transaction, r.transaction_uuid};
    cr.metadata.creator_local_transaction_id = r.transaction_order;
    cr.metadata.catalog_generation = std::max(base.catalog_generation, r.metadata.catalog_generation);
    cr.metadata.record.header.deleted = true;
    cr.metadata.lifecycle = c::CatalogObjectLifecycle::dropped;
    cr.metadata.status = c::CatalogObjectStatus::retired;
    cr.metadata.retired_transaction_uuid = {UuidKind::transaction, r.transaction_uuid};
    RoleScope scope(b, 7);
    const auto e = Oracle(b, file, cr);
    if (!e) return std::nullopt;
    file = e->file;
    stamped.insert(predecessor);
  }
  // The stamps: every comment tree leaf holding a base version's cell.
  const auto zero = d::DecodeFilespacePageZero(file.data(), b.size, nullptr);
  Check(zero.ok(), "page zero decodes for the oracle");
  const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& z) { return z.kind == 24; });
  Check(root != zero.record->roots.end(), "the comment tree root for the oracle");
  std::vector<u64> stack{root->page_number};
  std::size_t found = 0;
  while (!stack.empty()) {
    const auto n = stack.back();
    stack.pop_back();
    auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
    Check(page.ok(), "a comment tree page decodes for the oracle");
    if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
    bool changed = false;
    for (auto& cell : page.page->cells) {
      if (cell.child) stack.push_back(cell.child->page_number);
      if (!page.page->tree_level && stamped.contains(cell.key.version_uuid)) {
        cell.superseding_local_transaction_id = r.transaction_order;
        changed = true;
        ++found;
      }
    }
    if (!changed) continue;
    const auto image = pg::EncodeNativeBtreePage(*page.page);
    Check(image.ok(), "the oracle's stamped tree page encodes");
    std::copy(image.bytes.begin(), image.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(n * b.size));
  }
  if (found != stamped.size()) return std::nullopt;
  return file;
}
// Whether the committed read of a copy sees no live version of comment row k.
bool CommentInvisible(const Db& b, const std::string& path, unsigned k) {
  RoleScope scope(b, 7);
  const auto seen = Peek(b, path, CommentRowId(k));
  return seen.read && (!seen.version || seen.deleted);
}
// A refused successor or insert: nothing written, the file unchanged.
template <typename F>
db::CatalogVersionWriteResult Unwritten(const std::string& path, F&& write, std::string_view label) {
  const auto before = ReadAll(path);
  const auto r = write();
  Check(r.writes == 0 && r.syncs == 0 && !r.attempted && ReadAll(path) == before,
        std::string(label) + ": nothing written");
  return r;
}

void DropCascade(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto t = Template(b, b.bytes);
  const auto role_found = DefinitionRole(Places(b, b.bytes));
  Check(role_found.has_value(), "a definition relation for named objects");
  const u16 role = *role_found;
  const auto en = c::kCatalogLanguage_en, fr = c::kCatalogLanguage_fr_CA, de = c::kCatalogLanguage_de_DE;
  const auto path = b.Copy(b.bytes, "comment-drop.db");
  Named a, bb, cc;
  {
    Session s(b, path);
    Begin(s, 2);
    const auto file = ReadAll(path);
    const auto ms = After(b, file, role);
    a = NamedAfter(b, file, t, role, ms, 1, 2, "commented_object");
    bb = NamedAfter(b, file, t, role, ms, 2, 2, "uncommented_object");
    cc = NamedAfter(b, file, t, role, ms, 3, 2, "own_commented_object");
    unsigned k = 1;
    for (const auto* o : {&a, &bb, &cc}) {
      const auto r = InsertWithPages(b, s.sink(), 2, o->Rows(), k++);
      if (!r.ok()) std::cerr << "named insert: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok(), "a named object to comment on");
    }
    Commit(*s.w, s.sink(), 2);
    Begin(s, 3);
    const auto r = InsertWithPages(b, s.sink(), 3,
                                   {CommentOn(database, 3, 71, DefinitionObject(a), en, "The commented object."),
                                    CommentOn(database, 3, 72, DefinitionObject(a), fr, "L'objet comment\xc3\xa9.")},
                                   4);
    if (!r.ok()) std::cerr << "comments on a: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
    Check(r.ok(), "two comments (two languages) on a named definition");
    Commit(*s.w, s.sink(), 3);
  }
  ExpectComments(b, path, 2, "two comments on a named definition");
  const auto committed = ReadAll(path);

  // ---- Conflicts and target checks, on a copy ------------------------------------
  {
    const auto x = b.Copy(committed, "comment-drop-conflicts.db");
    Session s(b, x);
    // Another transaction's comment insert on the target, in progress.
    Begin(s, 4);
    Check(InsertWithPages(b, s.sink(), 4, {CommentOn(database, 4, 73, DefinitionObject(a), de, "Kommentar.")}, 5).ok(),
          "another transaction's comment on the target, in progress");
    Begin(s, 5);
    {
      const auto r = Unwritten(x, [&] { return db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, a, 5, 9701, 3)); },
                               "a drop while another transaction's comment insert is in progress");
      Check(r.error == CE::write_conflict && r.blocker_order == 4,
            "a drop while another transaction's comment insert is in progress: write_conflict naming it");
    }
    Rollback(*s.w, s.sink(), 4);
    // Another transaction's comment edit, in progress.
    Begin(s, 6);
    Check(db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), CommentSuccessor(b, ReadAll(x), 71, 6, 74, "Edited.")).ok(),
          "another transaction's comment edit, in progress");
    {
      const auto r = Unwritten(x, [&] { return db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, a, 5, 9702, 3)); },
                               "a drop while another transaction's comment edit is in progress");
      Check(r.error == CE::write_conflict && r.blocker_order == 6,
            "a drop while another transaction's comment edit is in progress: write_conflict naming it");
    }
    Rollback(*s.w, s.sink(), 6);
    // Another transaction's comment drop, in progress.
    Begin(s, 7);
    {
      const auto [predecessor, base] = NewestComment(b, ReadAll(x), 72);
      (void)base;
      Check(predecessor == CommentVersionId(72), "the French comment's newest version is its insert");
      Check(db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), CommentSuccessor(b, ReadAll(x), 72, 7, 75, std::nullopt, true)).ok(),
            "another transaction's comment drop, in progress");
    }
    {
      const auto r = Unwritten(x, [&] { return db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, a, 5, 9703, 3)); },
                               "a drop while another transaction's comment drop is in progress");
      Check(r.error == CE::write_conflict && r.blocker_order == 7,
            "a drop while another transaction's comment drop is in progress: write_conflict naming it");
    }
    Rollback(*s.w, s.sink(), 7);
    // Now 5 drops the target (its comments with it: rolled-back versions passed over).
    {
      const auto r = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, a, 5, 9704, 3));
      if (!r.ok()) std::cerr << "drop by 5: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok() && r.dependents.size() == 2, "the target dropped by 5, its two committed comments with it");
    }
    // Another transaction's comment insert while 5's drop of the target is in progress.
    Begin(s, 8);
    {
      const auto r = Unwritten(x, [&] { return InsertWithPages(b, s.sink(), 8, {CommentOn(database, 8, 76, DefinitionObject(a), de, "Zu sp\xc3\xa4t.")}, 6); },
                               "a comment insert while another transaction's drop of the target is in progress");
      Check(r.error == CE::write_conflict && r.blocker_order == 5,
            "a comment insert while another transaction's drop of the target is in progress: write_conflict naming it");
    }
    // 5 comments on what it dropped.
    {
      const auto r = Unwritten(x, [&] { return InsertWithPages(b, s.sink(), 5, {CommentOn(database, 5, 77, DefinitionObject(a), de, "Selbst.")}, 7); },
                               "a comment on the target this transaction dropped");
      Check(r.error == CE::missing_target, "a comment on the target this transaction dropped: missing_target");
    }
    Rollback(*s.w, s.sink(), 5);
    Rollback(*s.w, s.sink(), 8);
    // Another transaction's ALTER (a successor that is not a drop) of the
    // target in progress: a comment insert does not conflict (Q87 = 1).
    Begin(s, 9);
    {
      const auto r = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, bb, 9, 9705, 0, false));
      if (!r.ok()) std::cerr << "alter by 9: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok() && r.dependents.empty(), "another transaction alters the target (a successor that is not a drop)");
    }
    Begin(s, 10);
    {
      const auto r = InsertWithPages(b, s.sink(), 10, {CommentOn(database, 10, 78, DefinitionObject(bb), en, "During the alter.")}, 8);
      if (!r.ok()) std::cerr << "comment during alter: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok(), "a comment insert while another transaction's ALTER of the target is in progress succeeds (Q87 = 1)");
    }
    Commit(*s.w, s.sink(), 10);
    Commit(*s.w, s.sink(), 9);
    ExpectComments(b, x, 3, "a comment written during an ALTER, both committed");
    // This transaction's own uncommitted comment and its own drop of the
    // target. step4-open2 round 2, owner Q88 = 1 (2026-11-13;
    // SB_CATALOG_OWN_VERSION_REPLACE): a transaction's successor of its own
    // uncommitted version replaces it in place, so its edit of its own comment
    // and the drop's cascade over it are written. (This scenario asserted,
    // while the ruling was open, that both were refused: the edit
    // invalid_catalog at its cell's self-stamp, the drop invalid_successor.
    // Superseded by Q88 = 1.)
    Begin(s, 11);
    Check(InsertWithPages(b, s.sink(), 11, {CommentOn(database, 11, 79, DefinitionObject(cc), en, "Own.")}, 9).ok(),
          "a comment by the transaction that will drop its target");
    {
      const auto r = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), CommentSuccessor(b, ReadAll(x), 79, 11, 83, "Own, edited."));
      if (!r.ok()) std::cerr << "own edit: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok() && r.replaced && r.version_uuid == CommentVersionId(79),
            "Q88 = 1: an edit of the transaction's own uncommitted comment replaces it in place");
    }
    DropNames(b, s, cc, 11, 9710);
    {
      const auto r = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), DefinitionDrop(b, ReadAll(x), role, cc, 11, 9712, 0));
      if (!r.ok()) std::cerr << "own drop: " << db::CatalogVersionWriteErrorName(r.error) << '\n';
      Check(r.ok() && r.dependents.size() == 1 && r.dependents.front() == std::pair{CommentRowId(79), CommentVersionId(79)},
            "Q88 = 1: the drop takes the transaction's own comment, replacing it in place (no identity used)");
      RoleScope scope(b, 7);
      const auto rel = ReadRelation(b, ReadAll(x));
      const auto versions = rel.Versions(CommentRowId(79));
      const auto& m = rel.At(versions.front().first).metadata.at(CommentVersionId(79));
      Check(versions.size() == 1 && m.record.header.deleted && m.definition_version == 1,
            "the own comment's row holds one version, the drop, definition_version 1");
      const auto cells = CommentTreeCells(b, ReadAll(x));
      Check(std::none_of(cells.begin(), cells.end(), [](const auto& c) { return c.row == CommentRowId(79); }),
            "the own comment's tree cell is removed");
    }
    Commit(*s.w, s.sink(), 11);
    ExpectComments(b, x, 3, "an object dropped with its own comment, committed");
    Check(CommentInvisible(b, x, 79), "the own comment, dropped with its target, is not visible");
    // A committed-dropped target, and one that never existed.
    Begin(s, 12);
    {
      const auto r = Unwritten(x, [&] { return InsertWithPages(b, s.sink(), 12, {CommentOn(database, 12, 80, DefinitionObject(cc), en, "Gone.")}, 10); },
                               "a comment on a committed-dropped target");
      Check(r.error == CE::missing_target && r.failed_row == 0, "a comment on a committed-dropped target: missing_target");
      const auto nowhere = V7(kT + 990001, 0xc90001);
      const auto n = Unwritten(x, [&] { return InsertWithPages(b, s.sink(), 12, {CommentOn(database, 12, 81, nowhere, en, "None.")}, 11); },
                               "a comment on a missing target");
      Check(n.error == CE::missing_target, "a comment on a target that does not exist: missing_target");
      // A comment edit checks its target too: here it lives.
      Check(db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), CommentSuccessor(b, ReadAll(x), 78, 12, 82, "Edited after.")).ok(),
            "a comment edit whose target lives");
    }
    Commit(*s.w, s.sink(), 12);
    ExpectComments(b, x, 3, "after the refusals");
  }

  // ---- The drop of a commented object -----------------------------------------------
  Writes recorded;
  std::vector<byte> pre;
  const std::vector<std::pair<unsigned, Uuid>> comments{{71, DependentId(9720 * 10)}, {72, DependentId(9720 * 10 + 1)}};
  {
    Session s(b, path);
    Begin(s, 4);
    DropNames(b, s, a, 4, 9730);
    const auto now = ReadAll(path);
    {
      auto r = DefinitionDrop(b, now, role, a, 4, 9720, 1);
      const auto refused = Unwritten(path, [&] { return db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r); },
                                     "a drop with too few identities for its comments");
      Check(refused.error == CE::dependent_versions_exhausted && refused.cascade_needed == 2,
            "a drop with too few identities for its comments: dependent_versions_exhausted, two needed");
    }
    const auto r = DefinitionDrop(b, now, role, a, 4, 9720, 2);
    const auto expected = CascadeOracle(b, now, role, r, comments);
    Check(expected.has_value(), "the oracle places the drop and its comments' drops");
    pre = now;
    RecordingSink rec(s.sink());
    const auto written = db::WriteCatalogSuccessorVersion(rec, GrowingBinding(b), r);
    if (!written.ok()) std::cerr << "cascade drop: " << db::CatalogVersionWriteErrorName(written.error) << '\n';
    Check(written.ok() && written.allocated_pages.empty(), "the commented object's drop is written (no page added)");
    Check(written.dependents.size() == 2 && written.dependents[0] == std::pair{CommentRowId(71), comments[0].second} &&
              written.dependents[1] == std::pair{CommentRowId(72), comments[1].second},
          "its two comments dropped in the same write, in row-UUID order, with the request's identities");
    recorded = rec.writes;
    const auto after = ReadAll(path);
    Check(after == *expected, "the file is the oracle's: the definition's and both comments' drop versions, the cells stamped");
    bool tree_seen = false, leaf_after_tree = false;
    for (const auto& [offset, bytes] : recorded) {
      const auto type = TypeAt(b, after, offset / b.size);
      if (type >= 0x200 && type <= 0x202) tree_seen = true;
      else if (type == 6 && tree_seen) leaf_after_tree = true;
    }
    Check(tree_seen && !leaf_after_tree, "every base leaf (both relations) is written before any tree page");
    std::cout << "page_size=" << b.size << " drop with two comments: " << recorded.size() << " writes\n";
    Commit(*s.w, s.sink(), 4);
  }
  ExpectComments(b, path, 0, "a commented object dropped with its comments, committed");
  Check(CommentInvisible(b, path, 71) && CommentInvisible(b, path, 72), "the dropped comments are no longer visible");
  // The careful-write order of that drop: every prefix and each torn write.
  for (std::size_t k = 0; k <= recorded.size(); ++k) {
    const auto file = Apply(pre, recorded, k);
    const auto at = b.Copy(file, "comment-drop-prefix.db");
    ExpectComments(b, at, 2, "a crash after " + std::to_string(k) + " of the cascade's writes");
    if (k < recorded.size()) {
      const auto torn = Apply(pre, recorded, k, 1);
      const auto v = Verify(b, b.Copy(torn, "comment-drop-torn.db"), true);
      Check(v.refused || (v.full.ok() && v.full.comments.comments == 2),
            "a torn cascade write is refused at startup, or leaves a database that verifies with both comments live");
      std::cout << "page_size=" << b.size << " cascade write " << k << " of " << recorded.size() << " torn: "
                << (v.refused ? "refused" : "admitted, verifies") << '\n';
    }
  }
  // An uncommented object's drop: the successor oracle alone, no dependents.
  // On a copy of the file before the drop above (the definition relation's
  // room then is the room the oracle placed that drop in).
  {
    const auto y = b.Copy(committed, "comment-drop-uncommented.db");
    Session s(b, y);
    Begin(s, 4);
    DropNames(b, s, bb, 4, 9740);
    const auto r = DefinitionDrop(b, ReadAll(y), role, bb, 4, 9742, 2);
    RoleScope scope(b, role);
    auto plain = r;
    plain.dependent_versions.clear();
    const auto expected = Oracle(b, ReadAll(y), plain);
    Check(expected.has_value(), "the oracle places the uncommented drop");
    const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    Check(written.ok() && written.dependents.empty() && written.allocated_pages.empty() && ReadAll(y) == expected->file,
          "an uncommented object's drop is unchanged: the successor oracle's file, no dependents, identities unused");
    Commit(*s.w, s.sink(), 4);
    ExpectComments(b, y, 2, "an uncommented object dropped (the other object's comments untouched)");
  }
  std::cout << "page_size=" << b.size << " drop cascade: comments dropped with their target\n";
}

// ---- A transaction's successor of its own version (step4-open2 round 2) ------------
// SB_CATALOG_OWN_VERSION_REPLACE (owner Q88 = 1, 2026-11-13): a name or
// comment row's successor of the writing transaction's own uncommitted
// version replaces that version in place, FirebirdSQL style: the record in its
// slot rewritten (its version UUID, row_version and definition_version kept),
// its tree cells removed (written first) and, unless a drop, its new ones
// added (a comment edit keeps its cell: its key cannot change).
// The oracle: the leaf as read with that record's metadata replaced and
// re-encoded by the production encoders, the comment tree with the cell
// removed for a drop; every other byte unchanged.
std::vector<byte> CommentReplaceOracle(const Db& b, std::span<const byte> before, unsigned k, const Uuid& version,
                                       c::CatalogMetadataVersion metadata, bool remove_cell) {
  std::vector<byte> file(before.begin(), before.end());
  RoleScope scope(b, 7);
  const auto rel = ReadRelation(b, file);
  const auto at = rel.Find(version);
  const auto& base = rel.At(at.first).metadata.at(version);
  metadata.definition_version = base.definition_version;
  auto leaf = rel.At(at.first).leaf;
  Check(leaf.body.rows[at.second].row_uuid.value == CommentRowId(k), "the oracle finds the comment row");
  const auto encoded = c::EncodeCatalogMetadataVersion(metadata);
  Check(encoded.ok(), "the replacement metadata encodes");
  leaf.body.rows[at.second].cells.front().value.payload = encoded.bytes;
  const auto sealed = db::EncodeNativeCatalogLeaf(leaf);
  Check(sealed.ok(), "the oracle's leaf encodes");
  std::copy(sealed.bytes.begin(), sealed.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(at.first * b.size));
  if (remove_cell) {
    const auto zero = d::DecodeFilespacePageZero(file.data(), b.size, nullptr);
    const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& z) { return z.kind == 24; });
    std::vector<u64> stack{root->page_number};
    while (!stack.empty()) {
      const auto n = stack.back();
      stack.pop_back();
      auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
      Check(page.ok(), "a comment tree page decodes for the oracle");
      if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
      for (const auto& cell : page.page->cells)
        if (cell.child) stack.push_back(cell.child->page_number);
      if (page.page->tree_level) continue;
      const auto size = page.page->cells.size();
      std::erase_if(page.page->cells, [&](const auto& cell) { return cell.key.version_uuid == version; });
      if (page.page->cells.size() == size) continue;
      const auto image = pg::EncodeNativeBtreePage(*page.page);
      Check(image.ok(), "the oracle's tree page encodes");
      std::copy(image.bytes.begin(), image.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(n * b.size));
    }
  }
  return file;
}
db::CatalogVersionWriteResult ExpectCommentReplace(const Db& b, Session& s, const std::string& path, unsigned k, u64 n,
                                                   unsigned version_k, std::optional<std::string> text, bool drop,
                                                   std::string_view label) {
  const auto before = ReadAll(path);
  const auto [own, base] = NewestComment(b, before, k);
  (void)base;
  const auto r = CommentSuccessor(b, before, k, n, version_k, text, drop);
  const auto expected = CommentReplaceOracle(b, before, k, own, r.metadata, drop);
  const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
  if (!written.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok() && written.replaced && written.version_uuid == own && written.allocated_pages.empty(),
        std::string(label) + ": the own version is replaced in place (its version UUID kept)");
  Check(written.writes == (drop ? 2u : 1u), std::string(label) + (drop ? ": the tree page, then the leaf" : ": one leaf write"));
  Check(ReadAll(path) == expected, std::string(label) + ": the file is the oracle's");
  return written;
}
std::size_t VersionCount(const Db& b, std::span<const byte> file, u16 role, const Uuid& row) {
  RoleScope scope(b, role);
  return ReadRelation(b, file).Versions(row).size();
}
// The names relation's record of `version`, replaced as the writer must: the
// request's metadata (definition_version the replaced version's) with the
// name's envelope bound to the same page, slot, row_version and version.
std::vector<byte> NameReplaceOracle(const Db& b, std::span<const byte> before, const db::CatalogVersionRequest& r) {
  std::vector<byte> file(before.begin(), before.end());
  RoleScope scope(b, 2);
  const auto rel = ReadRelation(b, file);
  const auto at = rel.Find(r.predecessor_version_uuid);
  auto leaf = rel.At(at.first).leaf;
  const auto& row = leaf.body.rows[at.second];
  auto m = r.metadata;
  m.definition_version = rel.At(at.first).metadata.at(r.predecessor_version_uuid).definition_version;
  const c::CatalogNameVersionBinding binding{
      {UuidKind::database, leaf.header.database_uuid}, {UuidKind::filespace, leaf.header.filespace_uuid},
      {UuidKind::row, r.row_uuid}, {UuidKind::row, row.version_uuid}, m.record.header.object_uuid,
      {UuidKind::transaction, r.transaction_uuid}, leaf.body.page_number, row.stable_slot_id, leaf.header.page_generation,
      row.row_version, r.transaction_order, m.catalog_generation};
  const auto envelope = c::EncodeCatalogNameEnvelope({binding, *r.name});
  Check(envelope.ok(), "the oracle's name envelope encodes");
  m.record.payload.assign(envelope.bytes.begin(), envelope.bytes.end());
  const auto encoded = c::EncodeCatalogMetadataVersion(m);
  Check(encoded.ok(), "the oracle's name metadata encodes");
  leaf.body.rows[at.second].cells.front().value.payload = encoded.bytes;
  const auto sealed = db::EncodeNativeCatalogLeaf(leaf);
  Check(sealed.ok(), "the oracle's names leaf encodes");
  std::copy(sealed.bytes.begin(), sealed.bytes.end(), file.begin() + static_cast<std::ptrdiff_t>(at.first * b.size));
  return file;
}
// The name tree cells (kinds 22 and 23) as stored.
std::vector<std::pair<unsigned, CommentCell>> NameTreeCells(const Db& b, std::span<const byte> file) {
  const auto zero = d::DecodeFilespacePageZero(file.data(), b.size, nullptr);
  std::vector<std::pair<unsigned, CommentCell>> out;
  for (unsigned t = 0; t < 2; ++t) {
    const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [&](const auto& z) { return z.kind == 22 + t; });
    Check(root != zero.record->roots.end(), "page zero carries the name trees");
    std::vector<u64> stack{root->page_number};
    while (!stack.empty()) {
      const auto n = stack.back();
      stack.pop_back();
      const auto page = pg::DecodeNativeBtreePage(PageOf(file, n, b.size));
      Check(page.ok(), "a name tree page decodes");
      if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
      for (const auto& cell : page.page->cells) {
        if (cell.child) stack.push_back(cell.child->page_number);
        if (!page.page->tree_level)
          out.push_back({t, {cell.key.encoded_key, cell.key.row_uuid, cell.key.version_uuid, cell.creator_local_transaction_id,
                             cell.superseding_local_transaction_id}});
      }
    }
  }
  return out;
}
// A rename of the transaction's own entry version: the leaf the oracle's,
// one cell per tree for that version under the new name, created by n, the
// old name's authoritative key gone; every page outside the names relation's
// leaves and the two trees unchanged.
void ExpectNameReplace(const Db& b, Session& s, const std::string& path, const db::CatalogVersionRequest& r,
                       std::string_view label) {
  const auto before = ReadAll(path);
  const auto leaf_expected = NameReplaceOracle(b, before, r);
  const auto written = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
  if (!written.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(written.error) << '\n';
  Check(written.ok() && written.replaced && written.version_uuid == r.predecessor_version_uuid && written.allocated_pages.empty(),
        std::string(label) + ": the own entry version is replaced in place");
  const auto after = ReadAll(path);
  // The leaf: byte for byte. Tree pages: the cells below. Nothing else.
  std::set<u64> tree_pages;
  for (const auto* file : {&before, &after}) {
    const auto zero = d::DecodeFilespacePageZero(file->data(), b.size, nullptr);
    for (const auto& z : zero.record->roots)
      if (z.kind == 22 || z.kind == 23) {
        std::vector<u64> stack{z.page_number};
        while (!stack.empty()) {
          const auto n = stack.back();
          stack.pop_back();
          tree_pages.insert(n);
          const auto page = pg::DecodeNativeBtreePage(PageOf(*file, n, b.size));
          if (page.page->first_child) stack.push_back(page.page->first_child->page_number);
          for (const auto& cell : page.page->cells)
            if (cell.child) stack.push_back(cell.child->page_number);
        }
      }
  }
  for (u64 p = 0; p < after.size() / b.size; ++p) {
    if (tree_pages.contains(p)) continue;
    const auto o = static_cast<std::ptrdiff_t>(p * b.size);
    Check(std::equal(after.begin() + o, after.begin() + o + b.size, leaf_expected.begin() + o),
          std::string(label) + ": every page but the name trees' is the oracle's (page " + std::to_string(p) + ")");
  }
  const auto& e = std::get<c::CatalogNameEntry>(*r.name);
  const auto key = c::CatalogNameAuthoritativeTreeKey(c::BorrowCatalogNameEntry(e), e.object_kind);
  std::size_t mine[2] = {0, 0};
  bool keyed = false;
  for (const auto& [t, cell] : NameTreeCells(b, after))
    if (cell.row == r.row_uuid && cell.version == r.predecessor_version_uuid) {
      ++mine[t];
      Check(cell.creator == r.transaction_order && !cell.superseding, std::string(label) + ": the version's cells are its creator's, unstamped");
      if (t == 0) keyed = cell.key == key;
    }
  Check(mine[0] == 1 && mine[1] == 1 && keyed, std::string(label) + ": one cell per tree, the authoritative one under the new name");
}

void OwnReplace(const Db& b) {
  const auto database = DatabaseDefinition(b, b.bytes);
  const auto en = c::kCatalogLanguage_en, fr = c::kCatalogLanguage_fr_CA;
  const auto path = b.Copy(b.bytes, "own-replace.db");
  Writes recorded;
  std::vector<byte> pre;
  {
    Session s(b, path);
    // An own comment edited: replaced in place, its cell kept.
    Begin(s, 2);
    Check(InsertGrowing(b, s.sink(), 2, {CommentRow(database, 2, 91, en, "One.")}, 91).ok(), "an own comment");
    ExpectCommentReplace(b, s, path, 91, 2, 92, "Two.", false, "an edit of an own uncommitted comment");
    Commit(*s.w, s.sink(), 2);
  }
  ExpectComments(b, path, 1, "an own comment edited, committed");
  {
    const auto [version, m] = NewestComment(b, ReadAll(path), 91);
    Check(version == CommentVersionId(91) && m.definition_version == 1 && VersionCount(b, ReadAll(path), 7, CommentRowId(91)) == 1 &&
              c::DecodeCatalogComment(m.record.payload).record->comment == "Two.",
          "one version: the edit's text, definition_version 1");
  }
  {
    Session s(b, path);
    // A committed comment edited twice: the second replaces the first; rolled back.
    Begin(s, 3);
    Check(db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), CommentSuccessor(b, ReadAll(path), 91, 3, 93, "Three.")).ok(),
          "an edit of the committed comment (a new version)");
    ExpectCommentReplace(b, s, path, 91, 3, 94, "Four.", false, "a second edit by the same transaction");
    Rollback(*s.w, s.sink(), 3);
  }
  ExpectComments(b, path, 1, "two edits rolled back");
  {
    RoleScope scope(b, 7);
    PeekSees(b, path, CommentRowId(91), CommentVersionId(91), "rolled back: the committed version is visible again");
  }
  {
    Session s(b, path);
    Begin(s, 4);
    // The base is the committed version (the newest stored one is 3's,
    // rolled back).
    auto r = CommentSuccessor(b, ReadAll(path), 91, 4, 95, "Five.");
    {
      RoleScope scope(b, 7);
      const auto rel = ReadRelation(b, ReadAll(path));
      const auto at = rel.Find(CommentVersionId(91));
      auto m = rel.At(at.first).metadata.at(CommentVersionId(91));
      m.definition_version += 1;
      m.creator_transaction_uuid = {UuidKind::transaction, TxId(4)};
      m.creator_local_transaction_id = 4;
      m.record.payload = CommentPayload(CommentId(91), database.record.header.object_uuid.value, en, "Five.");
      r.metadata = m;
      r.predecessor_version_uuid = CommentVersionId(91);
    }
    const auto w = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!w.ok()) std::cerr << "edit after rollback: " << db::CatalogVersionWriteErrorName(w.error) << '\n';
    Check(w.ok() && !w.replaced, "an edit of the committed comment (a new version)");
    ExpectCommentReplace(b, s, path, 91, 4, 96, "Six.", false, "its second edit");
    Commit(*s.w, s.sink(), 4);
  }
  ExpectComments(b, path, 1, "two edits committed");
  {
    RoleScope scope(b, 7);
    PeekSees(b, path, CommentRowId(91), CommentVersionId(95), "committed: the replaced version is visible");
    const auto [version, m] = NewestComment(b, ReadAll(path), 91);
    Check(version == CommentVersionId(95) && m.definition_version == 2 && c::DecodeCatalogComment(m.record.payload).record->comment == "Six.",
          "its text the second edit's, definition_version 2");
  }
  // Insert then drop in one transaction: nothing live; on disk the row's one
  // version, the drop (definition_version 1), and no tree cell.
  {
    Session s(b, path);
    Begin(s, 5);
    Check(InsertGrowing(b, s.sink(), 5, {CommentRow(database, 5, 97, fr, "Bient\xc3\xb4t.")}, 97).ok(), "an own comment to drop");
    pre = ReadAll(path);
    RecordingSink rec(s.sink());
    const auto [own, base] = NewestComment(b, pre, 97);
    (void)base;
    const auto r = CommentSuccessor(b, pre, 97, 5, 98, std::nullopt, true);
    const auto expected = CommentReplaceOracle(b, pre, 97, own, r.metadata, true);
    const auto written = db::WriteCatalogSuccessorVersion(rec, GrowingBinding(b), r);
    Check(written.ok() && written.replaced && written.version_uuid == own && ReadAll(path) == expected,
          "insert then drop: the drop replaces the insert in place, the file the oracle's");
    recorded = rec.writes;
    Check(recorded.size() == 2 && TypeAt(b, ReadAll(path), recorded[0].first / b.size) >= 0x200 &&
              TypeAt(b, ReadAll(path), recorded[1].first / b.size) == 6,
          "the removed cell's tree page is written before the leaf");
    Commit(*s.w, s.sink(), 5);
  }
  ExpectComments(b, path, 1, "insert then drop, committed");
  Check(CommentInvisible(b, path, 97), "insert then drop: nothing visible");
  {
    RoleScope scope(b, 7);
    const auto rel = ReadRelation(b, ReadAll(path));
    const auto versions = rel.Versions(CommentRowId(97));
    const auto& m = rel.At(versions.front().first).metadata.at(CommentVersionId(97));
    const auto cells = CommentTreeCells(b, ReadAll(path));
    Check(versions.size() == 1 && m.record.header.deleted && m.definition_version == 1 &&
              std::none_of(cells.begin(), cells.end(), [](const auto& c) { return c.row == CommentRowId(97); }),
          "insert then drop: on disk one deleted version (definition_version 1), no tree cell");
  }
  // The careful-write order of that drop: every prefix, each write torn.
  for (std::size_t k = 0; k <= recorded.size(); ++k) {
    const auto at = b.Copy(Apply(pre, recorded, k), "own-replace-prefix.db");
    ExpectComments(b, at, 1, "a crash after " + std::to_string(k) + " of the replacing drop's writes");
    if (k < recorded.size()) {
      const auto v = Verify(b, b.Copy(Apply(pre, recorded, k, 1), "own-replace-torn.db"), true);
      Check(v.refused || (v.full.ok() && v.full.comments.comments == 1),
            "a torn replacing write is refused at startup, or verifies with the committed comment alone");
      std::cout << "page_size=" << b.size << " replacing drop write " << k << " of " << recorded.size() << " torn: "
                << (v.refused ? "refused" : "admitted, verifies") << '\n';
    }
  }
  // Forged states.
  const auto file = ReadAll(path);
  {
    // A cell left for the replaced (dropped) version: refused.
    auto forged = file;
    const auto zero = d::DecodeFilespacePageZero(forged.data(), b.size, nullptr);
    const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& z) { return z.kind == 24; });
    auto page = pg::DecodeNativeBtreePage(PageOf(forged, root->page_number, b.size));
    Check(page.ok() && page.page->tree_level == 0 && !page.page->cells.empty(), "the comment tree is one root leaf here");
    auto cell = page.page->cells.front();
    cell.key.row_uuid = CommentRowId(97);
    cell.key.version_uuid = CommentVersionId(97);
    const auto k = c::CatalogCommentTreeKey(database.record.header.object_uuid.value, fr);
    cell.key.encoded_key.assign(k.begin(), k.end());
    cell.creator_local_transaction_id = 5;
    cell.superseding_local_transaction_id = 0;
    page.page->cells.push_back(cell);
    std::sort(page.page->cells.begin(), page.page->cells.end(),
              [](const auto& x, const auto& y) { return pg::CompareNativeBtreeKeys(x.key, y.key) < 0; });
    const auto bytes = pg::EncodeNativeBtreePage(*page.page);
    Check(bytes.ok(), "the forged tree page encodes");
    std::copy(bytes.bytes.begin(), bytes.bytes.end(), forged.begin() + static_cast<std::ptrdiff_t>(root->page_number * b.size));
    const auto v = Verify(b, b.Copy(forged, "forged-replaced-cell.db"), true);
    Check(v.refused && v.refused->error == db::NativeCatalogCohortError::comment_tree_mismatch,
          "a cell left for a replaced (dropped) version is refused: comment_tree_mismatch");
  }
  {
    // Two versions of one comment row by one transaction (what appending an
    // own successor used to write), otherwise consistent: the second edit
    // appended after 4's committed version, with its own cell.
    auto forged = file;
    RoleScope scope(b, 7);
    const auto rel = ReadRelation(b, forged);
    const auto at = rel.Find(CommentVersionId(95));
    auto leaf = rel.At(at.first).leaf;
    auto m = rel.At(at.first).metadata.at(CommentVersionId(95));
    auto next = leaf.body.rows[at.second];
    const auto extra = V7(kT + 990002, 0xc90002);
    leaf.body.rows[at.second].next_version_uuid = extra;
    leaf.body.rows[at.second].next_row_version = next.row_version + 1;
    next.version_uuid = extra;
    next.previous_version_uuid = CommentVersionId(95);
    next.previous_row_version = next.row_version;
    next.row_version += 1;
    next.next_version_uuid = {};
    next.next_row_version = 0;
    u32 slot = 0;
    for (const auto& x : leaf.body.rows) slot = std::max(slot, x.stable_slot_id);
    next.stable_slot_id = slot + 1;
    m.definition_version += 1;
    m.record.payload = CommentPayload(CommentId(91), database.record.header.object_uuid.value, en, "Forged.");
    const auto encoded = c::EncodeCatalogMetadataVersion(m);
    Check(encoded.ok(), "the forged version encodes");
    next.cells.front().value.payload = encoded.bytes;
    leaf.body.rows.push_back(next);
    const auto sealed = db::EncodeNativeCatalogLeaf(leaf);
    Check(sealed.ok(), "the forged leaf encodes");
    std::copy(sealed.bytes.begin(), sealed.bytes.end(), forged.begin() + static_cast<std::ptrdiff_t>(at.first * b.size));
    const auto zero = d::DecodeFilespacePageZero(forged.data(), b.size, nullptr);
    const auto root = std::find_if(zero.record->roots.begin(), zero.record->roots.end(), [](const auto& z) { return z.kind == 24; });
    auto page = pg::DecodeNativeBtreePage(PageOf(forged, root->page_number, b.size));
    Check(page.ok() && page.page->tree_level == 0, "the comment tree is one root leaf here");
    auto cell = *std::find_if(page.page->cells.begin(), page.page->cells.end(),
                              [](const auto& x) { return x.key.version_uuid == CommentVersionId(95); });
    cell.key.version_uuid = extra;
    page.page->cells.push_back(cell);
    std::sort(page.page->cells.begin(), page.page->cells.end(),
              [](const auto& x, const auto& y) { return pg::CompareNativeBtreeKeys(x.key, y.key) < 0; });
    const auto bytes = pg::EncodeNativeBtreePage(*page.page);
    Check(bytes.ok(), "the forged tree page encodes");
    std::copy(bytes.bytes.begin(), bytes.bytes.end(), forged.begin() + static_cast<std::ptrdiff_t>(root->page_number * b.size));
    const auto v = Verify(b, b.Copy(forged, "forged-own-twice.db"), true);
    if (v.refused) std::cerr << "own twice: error=" << int(v.refused->error) << '\n';
    Check(v.refused && v.refused->error == db::NativeCatalogCohortError::own_version_repeated,
          "two versions of a comment row by one transaction are refused: own_version_repeated");
  }
  // Names: an own create then renamed; a committed name renamed twice
  // (rolled back, then committed).
  const auto t = Template(b, b.bytes);
  const auto role_found = DefinitionRole(Places(b, b.bytes));
  Check(role_found.has_value(), "a definition relation for named objects");
  Named o;
  const auto entry_of = [&](const std::string& text) {
    auto e = Keyed(std::get<c::CatalogNameEntry>(*o.entry.name), t, text);
    PathBeside(e, t.entry);
    return c::CatalogNamePayload{e};
  };
  {
    Session s(b, path);
    Begin(s, 6);
    const auto now = ReadAll(path);
    o = NamedAfter(b, now, t, *role_found, After(b, now, *role_found), 21, 6, "own_named_a");
    Check(InsertWithPages(b, s.sink(), 6, o.Rows(), 21).ok(), "an own named object");
    ExpectNameReplace(b, s, path, NameSuccessor(o.entry, o.entry.version_uuid, 6, VersionId(9801), entry_of("own_named_b"), false),
                      "an own create then renamed");
    Commit(*s.w, s.sink(), 6);
  }
  ExpectComments(b, path, 1, "an own create renamed, committed");
  Check(VersionCount(b, ReadAll(path), 2, o.entry.row_uuid) == 1, "the entry row holds one version");
  {
    // The committed entry's metadata, as read, is the base of the next renames.
    RoleScope scope(b, 2);
    const auto rel = ReadRelation(b, ReadAll(path));
    const auto at = rel.Find(o.entry.version_uuid);
    o.entry.metadata = rel.At(at.first).metadata.at(o.entry.version_uuid);
    o.entry.metadata.record.payload.clear();
    o.entry.name = entry_of("own_named_b");
  }
  for (const bool commit : {false, true}) {
    const u64 n = commit ? 8 : 7;
    Session s(b, path);
    Begin(s, n);
    const auto first = NameSuccessor(o.entry, o.entry.version_uuid, n, VersionId(9802 + 2 * static_cast<unsigned>(commit)),
                                     entry_of(commit ? "own_named_e" : "own_named_c"), false);
    const auto w = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), first);
    if (!w.ok()) std::cerr << "rename: " << db::CatalogVersionWriteErrorName(w.error) << '\n';
    Check(w.ok() && !w.replaced, "a rename of the committed name (a new version)");
    auto second = NameSuccessor(AsRow(first), first.version_uuid, n, VersionId(9803 + 2 * static_cast<unsigned>(commit)),
                                entry_of(commit ? "own_named_f" : "own_named_d"), false);
    ExpectNameReplace(b, s, path, second, "a rename of the own rename");
    if (commit) Commit(*s.w, s.sink(), n);
    else Rollback(*s.w, s.sink(), n);
  }
  ExpectComments(b, path, 1, "a rename of an own rename, rolled back then committed");
  {
    RoleScope scope(b, 2);
    PeekSees(b, path, o.entry.row_uuid, VersionId(9804), "the committed rename's version is visible");
  }
  std::cout << "page_size=" << b.size << " own-version replacement: comments and names\n";
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
    DropCascade(b);  // step4-open2
    OwnReplace(b);   // step4-open2 round 2 (owner Q88 = 1)
    std::cout << "PASS native catalog comment insert page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog comment insert profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
