// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_NAME_TREES
// The two name trees of a database the production create writes (owner
// 2026-11-09: names indexed for fast uuid<->name conversion, each row holding
// its full path), read back through the fixed-root lease and the cohort reader
// at every page size:
//   - page zero carries root kinds 22 and 23, each the root of a B-tree whose
//     pages carry the tree's dependency tuple (its issued index, the fixed key
//     profile of its form, the shared visibility profile, its issued map), and
//     the catalog holds those profile and map rows;
//   - the cohort reader reads both trees after the names relation and binds
//     every cell to a row version: the authoritative tree holds one cell per
//     name entry, the UUID-to-name tree one per name row; the counts, pages
//     and roots equal the create's receipt;
//   - every captured cell's key and payload are what catalog_name_tree_keys
//     derives from its row (the object's record kind from the row's
//     definition, the path's display components from the captured names);
//   - the trees' pages are captured as physical pages, so the startup
//     allocation check finds no orphan;
//   - without name-tree backing the read refuses (resource_exhausted);
//   - the sizing helper covers the trees the create built;
//   - the engine's metadata verification accepts the capture: the index
//     verifier resolves both trees' tuples to their map and key-profile rows,
//     the name tree verifier finds every cell's key and payload equal to what
//     its row derives and every name row's cells present; without the cells
//     captured it refuses (cells_not_captured);
//   - mutations of a copy: a cell's version changed, a cell's creator
//     changed, a cell removed (a row without its cell), root 23 removed from
//     page zero, the two roots' objects swapped (all refused by storage);
//     a cell's payload byte changed and a cell's key byte changed (bound by
//     storage, refused by the engine: cell_payload, cell_key).
#define SB_CATALOG_VERSION_WRITER_TEST_WITHOUT_MAIN
#include "native_catalog_version_writer_test.cpp"

#include "catalog_name_path.hpp"
#include "catalog_name_tree_keys.hpp"
#include "catalog_native_catalog_index.hpp"
#include "native_catalog_cohort_capture.hpp"
#include "native_catalog_cohort_lease_reader.hpp"
#include "engine/internal_api/verification/native_startup_catalog_allocation.hpp"
#include "engine/internal_api/catalog/native_catalog_metadata_verification.hpp"

namespace {
namespace ia = scratchbird::engine::internal_api;
using CoE = db::NativeCatalogCohortError;

struct Trees {
  // The lease's refusal, when the file does not get as far as the cohort read.
  std::optional<db::NativeFixedRootMemoryError> lease_error;
  db::NativeCatalogCohortInspection inspection;
  ia::NativeCatalogMetadataVerification metadata;  // the engine's verdict on the capture
  std::array<u64, 2> entries_and_vectors{};  // [0] entries, [1] vectors among the captured name rows
  std::size_t cells_captured = 0, cells_mismatched = 0, tree_pages_captured = 0;
  bool allocation_ok = false, maps_and_profiles_ok = false;
  std::array<std::optional<db::NativeCapturedNameTree>, 2> trees;
};

// Opens `path` read-only through the fixed-root lease and reads the cohort with
// or without name-tree backing and cell capture; checks the capture in place.
Trees Open(const Db& b, const std::string& path, bool with_trees, bool with_cells) {
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Trees out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    if (!fixed.ok()) {
      out.lease_error = fixed.error;
      Check(device.Close().ok(), "close");
      return out;
    }
    constexpr auto rows = db::kNativeCatalogScanRelationMaximumRows;
    const db::NativeCatalogRelationMemoryLimits limits{
        b.profile, 1, db::NativeCatalogScanBasePages(b.size), rows, rows, db::NativeCatalogScanNavigationLimits(b.size),
        with_trees ? db::NativeNameTreeNavigationLimits(b.size, rows) : db::NativeBtreeTreeMemoryLimits{}};
    auto cohort = db::PrepareNativeCatalogCohortRead(limits, g.memory, g.binding);
    Check(cohort.ok(), "cohort workspace");
    auto capture = db::PrepareNativeCatalogCapture({4096, 8u << 20, 1, 1024, 1024, 128, with_cells ? 8192u : 0u},
                                                   g.memory, g.binding);
    Check(capture.ok(), "capture workspace");
    out.inspection = db::NativeCatalogCohortLeaseReader::Read(*fixed.lease, 256u << 20, cohort.workspace, &capture.capture);
    if (!out.inspection.ok()) return out;
    const auto& cap = capture.capture;
    out.trees = cap.name_trees();
    for (const auto& row : cap.rows()) {
      if (!row.row.name_payload) continue;
      ++out.entries_and_vectors[std::holds_alternative<c::CatalogNameEntryView>(*row.row.name_payload) ? 0 : 1];
    }
    // The tuples name rows the catalog holds: a map per tree (role 2, the
    // names relation's constant identity, this tree's index) and the key
    // profile of its form.
    const auto definition = [&](const Uuid& id) -> const db::NativeCatalogVisibleRow* {
      for (const auto& row : cap.rows())
        if (!row.row.name_payload && row.row.metadata.record.header.object_uuid.value == id) return &row.row;
      return nullptr;
    };
    out.maps_and_profiles_ok = out.trees[0] && out.trees[1];
    for (unsigned t = 0; t < 2 && out.maps_and_profiles_ok; ++t) {
      const auto& d = out.trees[t]->dependencies;
      const auto* map_row = definition(d.dependency_map_uuid);
      const auto* key_row = definition(d.key_profile_uuid);
      if (!map_row || !key_row) { out.maps_and_profiles_ok = false; break; }
      const auto map = c::DecodeCatalogNativeCatalogIndexDependencyMap(map_row->metadata.record.payload);
      const auto key = c::DecodeCatalogNativeCatalogIndexKeyProfile(key_row->metadata.record.payload);
      out.maps_and_profiles_ok = map.ok() && key.ok() && map.record->relation_role == 2 &&
                                 map.record->relation_uuid == c::kNativeNameCatalogRelationUuid &&
                                 map.record->index_uuid == d.index_uuid && map.record->key_profile_uuid == d.key_profile_uuid &&
                                 d.key_profile_uuid == (t ? c::kNativeNameUuidTreeKeyProfileUuid : c::kNativeNameAuthoritativeTreeKeyProfileUuid) &&
                                 d.visibility_profile_uuid == c::kNativeCatalogIndexVisibilityProfileUuid &&
                                 key.record->key_form == (t ? c::NativeCatalogIndexKeyForm::name_uuid_v1 : c::NativeCatalogIndexKeyForm::name_authoritative_v1);
    }
    // Every captured cell against its row.
    if (with_cells) {
      c::CatalogNameIndex names;
      for (const auto& row : cap.rows()) if (row.row.name_payload) names.Add(*row.row.name_payload);
      const auto name_row = [&](const Uuid& version) -> const db::NativeCatalogVisibleRow* {
        for (const auto& row : cap.rows()) if (row.row.name_payload && row.row.version_uuid == version) return &row.row;
        return nullptr;
      };
      for (const auto& cell : cap.name_tree_cells()) {
        ++out.cells_captured;
        const auto* row = name_row(cell.version_uuid);
        if (!row || cell.creator_local_transaction_id != 1 || cell.superseding_local_transaction_id) { ++out.cells_mismatched; continue; }
        std::vector<byte> key, payload;
        if (const auto* e = std::get_if<c::CatalogNameEntryView>(&*row->name_payload)) {
          const auto* object = definition(e->object_uuid.value);
          if (!object) { ++out.cells_mismatched; continue; }
          const auto kind = object->metadata.record.header.kind;
          if (cell.tree == 0) { key = c::CatalogNameAuthoritativeTreeKey(*e, kind); payload = c::CatalogNameAuthoritativeTreePayload(*e); }
          else {
            key = c::CatalogNameUuidTreeEntryKey(*e, kind);
            const auto path = c::ResolveCatalogNamePath(*e, names);
            std::vector<std::string_view> components;
            for (const auto& component : path.components) components.push_back(component.display_name);
            payload = c::CatalogNameUuidTreeEntryPayload(e->name_entry_uuid.value, components);
          }
        } else {
          const auto& v = std::get<c::CatalogNameVectorView>(*row->name_payload);
          const auto* object = definition(v.object_uuid.value);
          if (!object || cell.tree != 1) { ++out.cells_mismatched; continue; }
          key = c::CatalogNameUuidTreeVectorKey(v.object_uuid.value, object->metadata.record.header.kind);
          payload = c::CatalogNameUuidTreeVectorPayload(v);
        }
        if (!std::equal(key.begin(), key.end(), cell.key.begin(), cell.key.end()) ||
            !std::equal(payload.begin(), payload.end(), cell.payload.begin(), cell.payload.end()))
          ++out.cells_mismatched;
      }
    }
    for (const auto& page : cap.physical_pages())
      if (out.trees[0] && out.trees[1] &&
          (page.owner_uuid == out.trees[0]->dependencies.index_uuid || page.owner_uuid == out.trees[1]->dependencies.index_uuid))
        ++out.tree_pages_captured;
    out.allocation_ok = ia::CheckNativeStartupCatalogAllocation(cap, fixed.lease->allocation()).ok();
    {
      const auto count = cap.rows().size();
      std::vector<std::size_t> order(count), parents(count);
      std::vector<byte> colors(count);
      out.metadata = ia::VerifyNativeCatalogMetadata(cap, {order, parents, colors});
    }
  }
  Check(device.Close().ok(), "close");
  return out;
}

// A B-tree page image of the file at `number`, decoded.
pg::NativeBtreePage TreePage(const Db& b, std::span<const byte> file, u64 number) {
  const auto image = file.subspan(number * b.size, b.size);
  const auto page = pg::DecodeNativeBtreePage(std::vector<byte>(image.begin(), image.end()));
  Check(page.ok(), "a name tree page decodes");
  return *page.page;
}
// The first leaf of the tree rooted at `root`.
u64 FirstLeaf(const Db& b, std::span<const byte> file, u64 root) {
  auto page = TreePage(b, file, root);
  while (page.tree_level > 0) {
    Check(page.first_child.has_value(), "a branch has a first child");
    const auto next = page.first_child->page_number;
    page = TreePage(b, file, next);
    if (page.tree_level == 0) return next;
  }
  return root;
}
void Mutations(const Db& b, const std::string& original) {
  const auto file = ReadAll(original);
  d::FileDevice device;
  Check(device.Open(original, d::FileOpenMode::open_existing_read_only).ok(), "open for page zero");
  const auto page_zero = d::ReadFilespacePageZeroFromOpenDevice(device);
  Check(page_zero.ok(), "page zero decodes");
  Check(device.Close().ok(), "close");
  const auto root_of = [&](u16 kind) {
    const auto at = std::find_if(page_zero.record->roots.begin(), page_zero.record->roots.end(),
                                 [&](const auto& r) { return r.kind == kind; });
    Check(at != page_zero.record->roots.end(), "page zero has the name tree root");
    return *at;
  };
  using NE = ia::NativeCatalogNameTreeVerificationError;
  const auto expect = [&](const std::vector<byte>& image, CoE error, std::string_view what,
                          std::optional<db::NativeFixedRootMemoryError> lease_error = std::nullopt,
                          std::optional<NE> engine_error = std::nullopt) {
    const auto path = b.Copy(image, "mutated.db");
    const auto opened = Open(b, path, true, engine_error.has_value());
    if (engine_error && opened.inspection.ok()) {
      if (opened.metadata.name_trees.error != *engine_error)
        std::cerr << what << ": engine name_trees error=" << int(opened.metadata.name_trees.error) << '\n';
      Check(!opened.metadata.ok() && opened.metadata.name_trees.error == *engine_error, what);
    }
    if (opened.lease_error != lease_error || (!lease_error && opened.inspection.error != error))
      std::cerr << what << ": lease=" << (opened.lease_error ? int(*opened.lease_error) : -1) << " error=" << int(opened.inspection.error)
                << " tree_error=" << int(opened.inspection.name_tree_error) << " page_error=" << int(opened.inspection.name_tree_page_error) << '\n';
    Check(opened.lease_error == lease_error && (lease_error || opened.inspection.error == error), what);
    fs::remove(path);
  };
  const auto with_page = [&](u64 number, const pg::NativeBtreePage& page) {
    const auto encoded = pg::EncodeNativeBtreePage(page);
    Check(encoded.ok(), "the mutated page encodes");
    auto image = file;
    std::copy(encoded.bytes.begin(), encoded.bytes.end(), image.begin() + static_cast<std::ptrdiff_t>(number * b.size));
    return image;
  };
  expect(file, CoE::none, "an unmutated copy opens");
  for (const u16 kind : {u16{22}, u16{23}}) {
    const auto leaf = FirstLeaf(b, file, root_of(kind).page_number);
    auto page = TreePage(b, file, leaf);
    Check(!page.cells.empty(), "the first leaf has cells");
    {
      auto changed = page;
      changed.cells.front().key.version_uuid.bytes[15] ^= 1;
      expect(with_page(leaf, changed), CoE::name_tree_mismatch, "a cell whose version is not a row version is refused");
    }
    {
      auto changed = page;
      changed.cells.front().creator_local_transaction_id = 2;
      expect(with_page(leaf, changed), CoE::name_tree_mismatch, "a cell whose creator is not its row's is refused");
    }
    {
      auto changed = page;
      changed.cells.pop_back();
      expect(with_page(leaf, changed), CoE::name_tree_mismatch, "a name row without its cell is refused");
    }
    {
      auto changed = page;
      changed.cells.front().payload.back() ^= byte{1};
      expect(with_page(leaf, changed), CoE::none, "a cell whose payload is not its row's is refused by the engine",
             std::nullopt, NE::cell_payload);
    }
    {
      // The key's last byte: the record kind (authoritative) or the name
      // class (UUID-to-name), neither of which reorders the leaf here.
      auto changed = page;
      changed.cells.front().key.encoded_key.back() ^= byte{1};
      const auto encoded = pg::EncodeNativeBtreePage(changed);
      if (encoded.ok())
        expect(with_page(leaf, changed), CoE::none, "a cell whose key is not its row's is refused by the engine",
               std::nullopt, NE::cell_key);
    }
  }
  {
    auto changed = *page_zero.record;
    std::erase_if(changed.roots, [](const auto& r) { return r.kind == 23; });
    const auto encoded = d::EncodeFilespacePageZero(changed);
    Check(encoded.ok(), "page zero without root 23 encodes");
    auto image = file;
    std::copy(encoded.bytes->begin(), encoded.bytes->end(), image.begin());
    expect(image, CoE::name_tree_failure, "one name tree root without the other is refused");
  }
  {
    auto changed = *page_zero.record;
    auto& first = *std::find_if(changed.roots.begin(), changed.roots.end(), [](const auto& r) { return r.kind == 22; });
    auto& second = *std::find_if(changed.roots.begin(), changed.roots.end(), [](const auto& r) { return r.kind == 23; });
    std::swap(first.object_uuid, second.object_uuid);
    const auto encoded = d::EncodeFilespacePageZero(changed);
    Check(encoded.ok(), "page zero with swapped tree objects encodes");
    auto image = file;
    std::copy(encoded.bytes->begin(), encoded.bytes->end(), image.begin());
    // The lease refuses first: page zero's root objects no longer match the
    // allocation records of the trees' root pages.
    expect(image, CoE::none, "page zero whose tree roots name each other's index is refused by the lease",
           db::NativeFixedRootMemoryError::root_mismatch);
  }
}
}  // namespace

int main() {
  try {
    for (unsigned index = 0; index < 5; ++index) {
      const auto b = ProductionDb(index);
      const auto original = (b->dir.path() / "created").string();
      const auto opened = Open(*b, original, true, true);
      Check(!opened.lease_error, "the fixed-root lease admits the production database");
      if (!opened.inspection.ok())
        std::cerr << "open error=" << int(opened.inspection.error) << " tree_error=" << int(opened.inspection.name_tree_error)
                  << " page_error=" << int(opened.inspection.name_tree_page_error) << " size=" << b->size << '\n';
      Check(opened.inspection.ok(), "the production database opens with its name trees");
      const auto& i = opened.inspection;
      Check(i.name_trees_present, "page zero carries both name trees");
      const auto entries = opened.entries_and_vectors[0], vectors = opened.entries_and_vectors[1];
      Check(entries > 1000 && vectors == entries, "the default pack names every object once, in one language");
      Check(i.name_tree_cells[0] == entries && i.name_tree_cells[1] == entries + vectors,
            "one authoritative cell per entry, one UUID-to-name cell per name row");
      Check(i.name_tree_cells == b->receipt.name_tree_cells && i.name_tree_pages == b->receipt.name_tree_pages,
            "the read trees are the create's receipt");
      for (unsigned t = 0; t < 2; ++t) {
        Check(b->receipt.name_tree_roots[t] && b->receipt.name_tree_roots[t]->kind == 22 + t, "the receipt names both roots");
        Check(opened.trees[t] && opened.trees[t]->root_kind == 22 + t &&
                  opened.trees[t]->dependencies.index_uuid == b->receipt.name_tree_roots[t]->object_uuid &&
                  opened.trees[t]->root.page_number == b->receipt.name_tree_roots[t]->page_number &&
                  opened.trees[t]->cells == i.name_tree_cells[t] && opened.trees[t]->pages == i.name_tree_pages[t],
              "the capture records each tree's root, tuple and counts");
        Check(db::NativeNameTreeNavigationLimits(b->size, db::kNativeCatalogScanRelationMaximumRows).maximum_pages >= i.name_tree_pages[t],
              "the sizing helper covers the tree the create built");
      }
      Check(opened.maps_and_profiles_ok, "the catalog holds each tree's dependency map and key profile row");
      Check(opened.cells_captured == i.name_tree_cells[0] + i.name_tree_cells[1] && opened.cells_mismatched == 0,
            "every captured cell's key and payload derive from its row");
      Check(opened.tree_pages_captured == i.name_tree_pages[0] + i.name_tree_pages[1], "the trees' pages are captured as physical pages");
      Check(opened.allocation_ok, "the startup allocation check finds no orphan");
      if (!opened.metadata.ok())
        std::cerr << "metadata: names=" << int(opened.metadata.names.error) << " paths=" << int(opened.metadata.paths.error)
                  << " indexes=" << int(opened.metadata.indexes.error) << " name_trees=" << int(opened.metadata.name_trees.error)
                  << " namespaces=" << int(opened.metadata.namespaces.error) << " audit=" << int(opened.metadata.audit.error)
                  << " security=" << int(opened.metadata.security.error) << " profiles=" << int(opened.metadata.naming_profiles.error)
                  << " collisions=" << int(opened.metadata.collisions.error) << '\n';
      Check(opened.metadata.ok(), "the engine's metadata verification accepts the production database");
      // The create indexes every non-empty relation and role 5: five of six.
      // step4: and role 7, the comments relation (empty, indexed): six of
      // seven, plus the comment tree's tuple.
      Check(opened.metadata.indexes.indexed_relations == 6 && opened.metadata.indexes.indexed_name_trees == 2 &&
                opened.metadata.indexes.indexed_comment_trees == 1,
            "step4: the index verifier resolves the six relation tuples, both name tree tuples and the comment tree's");
      Check(opened.metadata.name_trees.present && opened.metadata.name_trees.cells == opened.cells_captured &&
                opened.metadata.name_trees.rows == entries + vectors,
            "the name tree verifier compared every cell and every name row");
      {
        const auto without_cells = Open(*b, original, true, false);
        Check(without_cells.inspection.ok() && !without_cells.metadata.ok() &&
                  without_cells.metadata.name_trees.error == ia::NativeCatalogNameTreeVerificationError::cells_not_captured,
              "without the cells captured the engine refuses to verify the trees");
      }
      const auto without = Open(*b, original, false, false);
      Check(!without.lease_error && !without.inspection.ok() && without.inspection.error == CoE::resource_exhausted,
            "without name-tree backing the cohort read refuses");
      std::cout << "size " << b->size << ": entries " << entries << " trees " << i.name_tree_pages[0] << "+" << i.name_tree_pages[1]
                << " pages, " << i.name_tree_cells[0] << "+" << i.name_tree_cells[1] << " cells\n";
      if (index == 0) Mutations(*b, original);
    }
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  std::cout << "native_name_trees: " << checks << " checks passed\n";
  return 0;
}
