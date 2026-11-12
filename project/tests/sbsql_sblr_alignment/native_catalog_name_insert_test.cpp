// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_CATALOG_NAME_INSERT
// In-place writes W3b-1b-1: a transaction inserts a named object (definition,
// name vector, default name entry) in place, on databases written by the
// production create, one page size per run (--profile N). The databases,
// transactions, oracle and committed reads are those of
// native_catalog_insert_writer_test (and through it
// native_catalog_version_writer_test), included below.
//
// The named object is a generic policy row (a mechanics fixture) with its
// names built as the create builds them (BuildBootstrapNamedCreationUnit):
// both name rows copy the definition's metadata, the entry's keys are derived
// by DeriveCatalogIdentifierLookupKeysInto under the database's identifier
// profile, and the vector names the database's naming collision policy, all
// taken from a committed policy object's names (the template).
//
// Startup verification is run on a committed capture (the production cohort
// reader): name, naming-profile, collision and security-reference
// verification accept the committed named object, and full catalog
// verification refuses at exactly the audit verifier's event_binding (its
// rows cite the create's event, whose origin is transaction 1: the
// catalog-change audit event comes with W3b-2, and this is its flip target).
// [W3b-2b: flipped. Every transaction writes its catalog.metadata_change event
// at BEGIN and every definition cites it: full verification passes.]
//   - A named object by a transaction in progress (not visible), committed
//     (visible; verifiers as above).
//   - Collisions: the template's own name, and the same name in another case
//     when the identifier profile folds it, refuse duplicate_name; a name
//     another transaction in progress inserted is write_conflict naming it,
//     insertable once it rolls back, a duplicate once it commits; two entries
//     of one request with one key; an alias of the same object is admitted.
//   - Refusals before any write: keys, profiles, supplements, policy, vector,
//     relation rules, name/payload presence, binding, database kind.
//   - One class in one scope under a second collision policy the transaction
//     inserted: invalid_name (startup's conflicting_policy).
//   - Name rows at keys between two keys and below the smallest of the names
//     relation (index_full where its leftmost leaf is full).
// Where the names relation has no room at a page size (no reached leaf, or a
// full rightmost index leaf), the named insert refuses relation_full or
// index_full with no write and the scenarios that need a committed named
// object report that; refusals that precede placement are still run.
#define SB_CATALOG_INSERT_WRITER_TEST_WITHOUT_MAIN
#include "native_catalog_insert_writer_test.cpp"

#include "catalog_construction_intent.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_metadata_change.hpp"
#include <functional>
#include "catalog_identifier_profile.hpp"
#include "catalog_naming_collision_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include "catalog_records.hpp"
#include <memory>
#include "native_catalog_cohort_capture.hpp"
#include "native_catalog_cohort_lease_reader.hpp"
#include "engine/internal_api/catalog/native_catalog_metadata_verification.hpp"
#include "engine/internal_api/verification/native_startup_catalog_allocation.hpp"

namespace {
namespace ia = scratchbird::engine::internal_api;

// ---- The template: a committed policy object's names -----------------------------------
struct NameTemplate {
  c::CatalogNameVector vector;
  c::CatalogNameEntry entry;
  // The decoded profile borrows its text from the payload it came from: the
  // payload is held here, on the heap, for the template's life.
  std::shared_ptr<const std::string> identifier_payload;
  c::CatalogIdentifierResolutionProfile identifier;
  Uuid object;
};
// Every name in the names relation, its envelope decoded against where it is.
struct StoredNames {
  std::vector<c::CatalogNameVector> vectors;
  std::vector<c::CatalogNameEntry> entries;
};
StoredNames NamesOf(const Db& b, std::span<const byte> file) {
  RoleScope scope(b, 2);
  const auto rel = ReadRelation(b, file);
  StoredNames out;
  for (const auto& l : rel.leaves)
    for (const auto& row : l.leaf.body.rows) {
      const auto& m = l.metadata.at(row.version_uuid);
      const c::CatalogNameVersionBinding at{
          {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
          row.row_uuid, {UuidKind::row, row.version_uuid}, m.record.header.object_uuid, row.transaction_uuid, l.page,
          row.stable_slot_id, row.storage_generation, row.row_version, row.local_transaction_id, m.catalog_generation};
      const std::vector<byte> bytes(reinterpret_cast<const byte*>(m.record.payload.data()),
                                    reinterpret_cast<const byte*>(m.record.payload.data()) + m.record.payload.size());
      const auto decoded = c::DecodeCatalogNameEnvelope(bytes, at);
      Check(decoded.ok(), "every stored name decodes against where it is");
      if (const auto* v = std::get_if<c::CatalogNameVector>(&decoded.record->payload)) out.vectors.push_back(*v);
      if (const auto* e = std::get_if<c::CatalogNameEntry>(&decoded.record->payload)) out.entries.push_back(*e);
    }
  return out;
}
NameTemplate Template(const Db& b, std::span<const byte> file) {
  const auto names = NamesOf(b, file);
  for (const auto& e : names.entries) {
    if (e.object_kind != c::CatalogRecordKind::policy || !e.default_for_object || e.lifecycle_state != c::CatalogNameLifecycle::active) continue;
    const auto v = std::find_if(names.vectors.begin(), names.vectors.end(),
                                [&](const auto& x) { return x.name_vector_uuid.value == e.name_vector_uuid.value; });
    if (v == names.vectors.end()) continue;
    RoleScope scope(b, 5);
    const auto profiles = ReadRelation(b, file);
    for (const auto& l : profiles.leaves)
      for (const auto& [version, m] : l.metadata)
        if (m.record.header.object_uuid.value == e.identifier_profile_uuid.value) {
          auto payload = std::make_shared<const std::string>(m.record.payload);
          const auto profile = c::DecodeCatalogIdentifierProfile(*payload);
          Check(profile.ok(), "the template's identifier profile decodes");
          return {*v, e, payload, *profile.record, e.object_uuid.value};
        }
  }
  throw std::runtime_error("no named policy object to take names from");
}

// ---- A named object --------------------------------------------------------------------
struct Named {
  db::CatalogInsertRow definition, vector, entry;
  std::vector<db::CatalogInsertRow> Rows() const { return {definition, vector, entry}; }
  // The entry first: its checks run before anything is placed (the writer
  // finds its vector anywhere in the request), so a refusal does not depend
  // on room.
  std::vector<db::CatalogInsertRow> EntryFirst() const { return {entry, vector, definition}; }
};
c::CatalogNameEntry Keyed(c::CatalogNameEntry e, const NameTemplate& t, const std::string& text) {
  e.raw_name_text = text;
  e.was_quoted = false;
  e.quote_style = c::CatalogNameQuoteStyle::none;
  e.requires_exact_match = false;
  const c::CatalogIdentifierKeyRequest request{&t.identifier, text, false, c::CatalogNameQuoteStyle::none, false};
  std::vector<byte> backing(2 * text.size() + 2);
  const auto derived = c::DeriveCatalogIdentifierLookupKeysInto(request, backing);
  if (!derived.ok()) std::cerr << "key derivation error=" << int(derived.error) << " text=" << text << '\n';
  Check(derived.ok(), "the name's keys derive");
  e.normalized_lookup_key.assign(derived.normalized.begin(), derived.normalized.end());
  e.exact_lookup_key.assign(derived.exact.begin(), derived.exact.end());
  if (t.identifier.storage_display_mode == c::IdentifierDisplayMode::preserve_user_text) e.display_name = text;
  else e.display_name.assign(derived.normalized.begin(), derived.normalized.end());
  return e;
}
// SB_NATIVE_NAME_PATH, written out here rather than called: a component's own
// lane key, escaped (0x00 -> 0x00 0xFF, then 0x00 0x00), root side first.
std::vector<byte> Escaped(std::span<const byte> raw) {
  std::vector<byte> out;
  for (const byte b : raw) {
    out.push_back(b);
    if (!b) out.push_back(0xff);
  }
  out.push_back(0);
  out.push_back(0);
  return out;
}
std::span<const byte> OwnKey(const c::CatalogNameEntry& e) {
  return e.requires_exact_match ? std::span<const byte>(e.exact_lookup_key) : std::span<const byte>(e.normalized_lookup_key);
}
// The full path of `e`, named in the same scope and language as `neighbour`:
// the neighbour's path without its own component, then e's own key.
void PathBeside(c::CatalogNameEntry& e, const c::CatalogNameEntry& neighbour) {
  const auto last = Escaped(OwnKey(neighbour));
  const auto& path = neighbour.full_path_lookup_key;
  Check(neighbour.path_component_count >= 1 && path.size() >= last.size() &&
            std::equal(last.begin(), last.end(), path.end() - static_cast<std::ptrdiff_t>(last.size())),
        "a stored name's full path ends with its own key");
  e.full_path_lookup_key.assign(path.begin(), path.end() - static_cast<std::ptrdiff_t>(last.size()));
  const auto own = Escaped(OwnKey(e));
  e.full_path_lookup_key.insert(e.full_path_lookup_key.end(), own.begin(), own.end());
  e.path_component_count = neighbour.path_component_count;
}
// The definition in `role` at row `key`; its vector and entry in the names
// relation at `vector_key` and `entry_key`. Its parent and schema are the
// template object's, so its name resolves in the template's scope.
Named NamedObject(const Db& b, std::span<const byte> file, const NameTemplate& t, u16 role, const Uuid& key,
                  const Uuid& vector_key, const Uuid& entry_key, unsigned k, u64 n, const std::string& text) {
  Named out;
  out.definition = Row(b, file, role, key, k, n);
  auto& d = out.definition.metadata;
  d.record.header.parent_uuid = {UuidKind::object, t.entry.scope_uuid.value};
  d.owning_schema_uuid = t.entry.parent_schema_uuid ? scratchbird::core::platform::TypedUuid{UuidKind::schema, t.entry.parent_schema_uuid->value}
                                                    : scratchbird::core::platform::TypedUuid{};
  const auto object = d.record.header.object_uuid;
  const scratchbird::core::platform::TypedUuid vector{UuidKind::object, V7(Ms(key), 0x710000 + k)};
  const scratchbird::core::platform::TypedUuid entry{UuidKind::object, V7(Ms(key), 0x720000 + k)};
  d.name_vector_uuid = vector;
  d.default_name_uuid = entry;
  d.retired_transaction_uuid = {};  // live: no retiring transaction

  auto vm = d;
  vm.record.header.kind = c::CatalogRecordKind::localized_name;
  vm.record.header.object_uuid = vector;
  vm.record.header.row_uuid = {UuidKind::row, vector_key};
  vm.record.header.parent_uuid = object;
  vm.record.payload.clear();
  vm.object_subtype = "name_vector";
  auto em = d;
  em.record.header.kind = c::CatalogRecordKind::localized_name;
  em.record.header.object_uuid = entry;
  em.record.header.row_uuid = {UuidKind::row, entry_key};
  em.record.header.parent_uuid = vector;
  em.record.payload.clear();
  em.object_subtype = "name_entry";
  em.resource_epoch = t.entry.resource_epoch;  // the name key's epoch, as the create sets it

  auto v = t.vector;
  v.name_vector_uuid = vector;
  v.object_uuid = object;
  if (d.owning_schema_uuid.value.is_nil()) v.owning_schema_uuid.reset();
  else v.owning_schema_uuid = d.owning_schema_uuid;
  v.default_name_entry_uuid = entry;
  v.catalog_generation_id = vm.catalog_generation;
  v.security_policy_uuid = d.security_policy_uuid;
  v.lifecycle_state = c::CatalogNameLifecycle::active;

  auto e = Keyed(t.entry, t, text);
  e.name_entry_uuid = entry;
  e.name_vector_uuid = vector;
  e.object_uuid = object;
  e.scope_uuid = d.record.header.parent_uuid;
  if (d.owning_schema_uuid.value.is_nil()) e.parent_schema_uuid.reset();
  else e.parent_schema_uuid = d.owning_schema_uuid;
  e.parent_object_uuid.reset();
  e.language_uuid = v.default_language_uuid;
  e.name_class = c::CatalogNameClass::primary;
  e.catalog_generation_id = em.catalog_generation;
  e.created_transaction_uuid = d.creator_transaction_uuid;
  e.security_policy_uuid = d.security_policy_uuid;
  e.resource_epoch = em.resource_epoch;
  e.default_for_language = true;
  e.default_for_object = true;
  e.lifecycle_state = c::CatalogNameLifecycle::active;
  e.dropped_transaction_uuid.reset();
  PathBeside(e, t.entry);

  out.vector = {2, vector_key, VersionId(k + 3000), vm, c::CatalogNamePayload{v}};
  out.entry = {2, entry_key, VersionId(k + 6000), em, c::CatalogNamePayload{e}};
  Check(c::CatalogNamePayloadMatchesMetadata(*out.vector.name, vm) &&
            c::CatalogNamePayloadMatchesMetadata(*out.entry.name, em),
        "the named object's names agree with their metadata");
  return out;
}
// Another entry (an alias, not the default) for the object `of` names.
db::CatalogInsertRow Alias(const Named& of, const NameTemplate& t, const Uuid& key, unsigned k, u64 n,
                           const std::string& text) {
  auto row = of.entry;
  row.row_uuid = key;
  row.version_uuid = VersionId(k + 6000);
  const scratchbird::core::platform::TypedUuid id{UuidKind::object, V7(Ms(key), 0x730000 + k)};
  row.metadata.record.header.object_uuid = id;
  row.metadata.record.header.row_uuid = {UuidKind::row, key};
  row.metadata.default_name_uuid = id;
  row.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  row.metadata.creator_local_transaction_id = n;
  auto e = Keyed(std::get<c::CatalogNameEntry>(*of.entry.name), t, text);
  e.name_entry_uuid = id;
  e.name_class = c::CatalogNameClass::alias;
  e.default_for_object = false;
  e.default_for_language = false;
  e.created_transaction_uuid = row.metadata.creator_transaction_uuid;
  PathBeside(e, std::get<c::CatalogNameEntry>(*of.entry.name));
  row.name = c::CatalogNamePayload{e};
  Check(c::CatalogNamePayloadMatchesMetadata(*row.name, row.metadata), "the alias agrees with its metadata");
  return row;
}

// ---- Startup verification on a committed capture -----------------------------------------
struct Verified {
  ia::NativeCatalogMetadataVerification full;
  ia::NativeCatalogNameTreeVerification name_trees;  // SB_NATIVE_NAME_TREES, run on its own
  ia::NativeCatalogNameVerification names;
  ia::NativeCatalogSecurityReferenceVerification security;
  ia::NativeCatalogNamingProfileVerification profiles;
  ia::NativeCatalogCollisionVerification collisions;
  // The startup allocation check over the capture's physical pages: every
  // page the allocation map holds that the capture reaches matches it, and
  // every captured page is matched (SB_CATALOG_NEW_PAGES: system pages too).
  bool allocation = false;
  // Set when the caller admits a refused capture and it was refused.
  std::optional<db::NativeCatalogCohortInspection> refused;
};
// On a copy of the file as it stands (a writing session keeps its device open).
// `tier`: 1 is the startup complete-scan tier; a larger value is a TEST-ONLY
// tier (that many times its bytes and rows), so the verifiers can be run over a
// names relation grown past the startup tier. The startup tier's refusal of
// such a relation is asserted separately (ExpectNamedVerification).
Verified Verify(const Db& b, const std::string& original, bool admit_refusal = false, usize tier = 1) {
  const auto path = b.Copy(ReadAll(original), "verify.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Verified out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);  // the page-buffer pool limit of the default memory policy
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    Check(fixed.ok(), "the fixed-root lease admits the file");
    // The shared complete-scan tier and the capture bounds startup uses.
    // SB_NATIVE_NAME_TREES: the production database has the name trees.
    const usize rows = db::kNativeCatalogScanRelationMaximumRows * tier;
    const db::NativeCatalogRelationMemoryLimits limits{b.profile, 1,
                                                       db::NativeCatalogBasePagesFor(b.size, db::kNativeCatalogScanRelationMaximumBytes * tier),
                                                       rows, rows, db::NativeCatalogNavigationLimitsFor(b.size, rows),
                                                       db::NativeNameTreeNavigationLimits(b.size, rows)};
    auto cohort = db::PrepareNativeCatalogCohortRead(limits, g.memory, g.binding);
    Check(cohort.ok(), "cohort workspace");
    // SB_NATIVE_NAME_TREES: the cells too, for the engine's name tree check.
    // The capture's page bounds cover the file: a database larger than the
    // fixture's (1,024 pages at 8 KiB) has more catalog and initial control
    // (allocation map) pages.
    const usize file_pages = b.bytes.size() / b.size;
    const bool larger = file_pages > 1024;
    auto capture = db::PrepareNativeCatalogCapture({4096 * tier, (8u << 20) * tier, 1, 1024, larger ? file_pages : 1024,
                                                    larger ? file_pages : 128, 8192 * tier},
                                                   g.memory, g.binding);
    Check(capture.ok(), "capture workspace");
    const auto read = db::NativeCatalogCohortLeaseReader::Read(*fixed.lease, 256u << 20, cohort.workspace, &capture.capture);
    if (!read.ok())
      std::cerr << "cohort read error=" << int(read.error) << " relation_error=" << int(read.relation_error)
                << " visibility_error=" << int(read.visibility_error) << " role=" << read.failing_role
                << " name_tree_error=" << int(read.name_tree_error) << '\n';
    if (!read.ok() && admit_refusal) {
      out.refused = read;
    } else {
    Check(read.ok(), "the committed catalog captures");
    const auto count = capture.capture.rows().size();
    std::vector<std::size_t> order(count), parents(count);
    std::vector<byte> colors(count);
    const ia::NativeCatalogNamespaceWorkspace workspace{order, parents, colors};
    out.full = ia::VerifyNativeCatalogMetadata(capture.capture, workspace);
    out.name_trees = ia::VerifyNativeCatalogNameTrees(capture.capture, workspace);
    out.names = ia::VerifyNativeCatalogNameBindings(capture.capture, order);
    out.security = ia::VerifyNativeCatalogSecurityReferences(capture.capture, order);
    out.profiles = ia::VerifyNativeCatalogNamingProfileReferences(capture.capture, order);
    out.collisions = ia::VerifyNativeCatalogNamingCollisions(capture.capture, workspace);
    const auto coverage = ia::CheckNativeStartupCatalogAllocation(capture.capture, fixed.lease->allocation());
    out.allocation = coverage.ok() && coverage.matched_pages == capture.capture.physical_pages().size();
    }
  }
  Check(device.Close().ok(), "close the verifier's open");
  fs::remove(path);
  return out;
}
// A committed named object: every verifier but audit accepts it, and full
// verification refuses at exactly audit event_binding (W3b-2's flip target).
// [W3b-2b: flipped; see the body: every committed change is audited now.]
// `full_passes`: the audit verifier judges live versions (W3b-2 adds the
// event every change cites); a drop leaves none of the object's live, so full
// verification passes for it.
// `admit_scan_bound`: the startup scan tier bounds a relation at 4 MiB
// (kNativeCatalogScanRelationMaximumBytes); a names relation grown past it
// is refused there, resource_exhausted for role 2, and nothing else is.
void ExpectNamedVerification(const Db& b, const std::string& path, std::size_t definitions, std::string_view label,
                             bool full_passes = false, bool admit_scan_bound = false) {
  // W3b-2b: every transaction writes its catalog.metadata_change event at
  // BEGIN and every definition cites it, so every committed state is audited
  // and FULL verification passes (until W3b-2 it refused at audit
  // event_binding; `full_passes` no longer selects anything). `definitions`
  // counts the named definitions; name verification also counts each event
  // row as an unnamed definition: the events the audit verifier counts beyond
  // the create's construction intent are added.
  (void)full_passes;
  auto v = Verify(b, path, admit_scan_bound);
  if (v.refused) {
    Check(v.refused->error == db::NativeCatalogCohortError::relation_failure &&
              v.refused->relation_error == db::NativeCatalogRelationLeaseError::resource_exhausted && v.refused->failing_role == 2,
          std::string(label) + ": refused only as the names relation past the startup scan tier");
    std::cout << b.name << " page_size=" << b.size << " " << label
              << ": the names relation is past the startup scan tier (4 MiB): capture refused resource_exhausted;"
                 " verified again on a TEST-ONLY tier twice its size\n";
    v = Verify(b, path, false, 2);
  }
  if (!v.full.ok())
    std::cerr << label << " full: names=" << int(v.full.names.error) << " indexes=" << int(v.full.indexes.error)
              << " namespaces=" << int(v.full.namespaces.error) << " filespaces=" << int(v.full.filespaces.error)
              << " audit=" << int(v.full.audit.error) << " security=" << int(v.full.security.error)
              << " profiles=" << int(v.full.naming_profiles.error) << " collisions=" << int(v.full.collisions.error)
              << " paths=" << int(v.full.paths.error) << " name_trees=" << int(v.full.name_trees.error) << '\n';
  Check(v.full.ok(), std::string(label) + ": full verification passes (every committed change audited)");
  const std::size_t events = v.full.audit.events ? v.full.audit.events - 1 : 0;
  if (!v.names.ok() || v.names.definitions != definitions + events)
    std::cerr << label << " names=" << int(v.names.error) << " definitions=" << v.names.definitions << " expected "
              << definitions << " + " << events << " events\n";
  Check(v.names.ok() && v.names.definitions == definitions + events, std::string(label) + ": name verification accepts it");
  Check(v.profiles.ok() && v.collisions.ok() && v.security.ok(),
        std::string(label) + ": naming-profile, collision and security verification accept it");
  Check(v.allocation, std::string(label) + ": every captured catalog page is an allocated page the map records");
  Check(v.name_trees.ok() && v.name_trees.present, std::string(label) + ": the name trees agree with the names");
}

// ---- Choosing keys ----------------------------------------------------------------------
// Keys after every key of the definition's relation and of the names relation.
u64 After(const Db& b, std::span<const byte> file, u16 role) {
  u64 ms = 0;
  for (const u16 r : {role, u16{2}}) {
    RoleScope scope(b, r);
    const auto tree = ReadTree(b, file);
    for (const auto& p : tree.pages)
      for (const auto& cell : p.page.cells)
        if (!p.page.tree_level) ms = std::max(ms, Ms(cell.key.row_uuid));
  }
  return ms + 1;
}
Named NamedAfter(const Db& b, std::span<const byte> file, const NameTemplate& t, u16 role, u64 ms, unsigned k, u64 n,
                 const std::string& text) {
  return NamedObject(b, file, t, role, V7(ms + k, 0xa00000 + k), V7(ms + k, 0xb00000 + k), V7(ms + k, 0xc00000 + k), k,
                     n, text);
}
// A definition relation with room for the fixture definition (role 5 first:
// the create keeps its policies there), or nullopt.
std::optional<u16> DefinitionRole(const std::vector<Place>& places) {
  for (const u16 role : {u16{5}, u16{4}, u16{6}})
    if (std::any_of(places.begin(), places.end(), [&](const Place& p) { return p.role == role; })) return role;
  return std::nullopt;
}

// ---- A named object, by state ------------------------------------------------------------
// Returns whether the names relation had room (the later scenarios need a
// committed named object).
bool NamedStates(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "named.db");
  Session s(b, path);
  Begin(s, 2);
  const auto file = ReadAll(path);
  const auto named = NamedAfter(b, file, t, role, After(b, file, role), 1, 2, "w3b_named_object");
  const auto request = Request(2, named.Rows());
  if (!InsertOracle(b, file, request)) {
    const auto result = Insert(b, s.sink(), request);
    Check((result.error == CE::relation_full || result.error == CE::index_full) && result.writes == 0 &&
              ReadAll(path) == file,
          "no room in the names relation: refused with no write");
    std::cout << b.name << " named page_size=" << b.size << " no room: " << db::CatalogVersionWriteErrorName(result.error)
              << " (W3b-3)\n";
    fs::remove(path);
    return false;
  }
  const auto writes = ExpectInsert(b, s, path, request, "a named object by a transaction in progress");
  Check(writes.size() >= 3, "the definition's leaf, the names' leaf, then their index leaves");
  {
    RoleScope a(b, 2);
    const auto seen = Peek(b, path, named.entry.row_uuid);
    Check(seen.read && !seen.version, "names in progress are not visible");
  }
  Commit(*s.w, s.sink(), 2);
  {
    RoleScope a(b, role);
    PeekSees(b, path, named.definition.row_uuid, named.definition.version_uuid, "the definition, committed");
  }
  {
    RoleScope a(b, 2);
    PeekSees(b, path, named.vector.row_uuid, named.vector.version_uuid, "the vector, committed");
    PeekSees(b, path, named.entry.row_uuid, named.entry.version_uuid, "the entry, committed");
  }
  ExpectNamedVerification(b, path, definitions + 1, "a committed named object");
  // An alias of the same object, same name: admitted (one object).
  Begin(s, 3);
  {
    const auto now = ReadAll(path);
    const auto alias = Alias(named, t, V7(After(b, now, role), 0xd00001), 2, 3, "w3b_named_object");
    ExpectInsert(b, s, path, Request(3, {alias}), "an alias of the same object, same name");
    Commit(*s.w, s.sink(), 3);
    ExpectNamedVerification(b, path, definitions + 1, "an alias, committed");
  }
  fs::remove(path);
  return true;
}

// ---- New base leaves (W3b-3a) ---------------------------------------------
// Fails the first write outside the allocation map: a crash after a write's
// allocation and before its own plan.
class FailAfterAllocation final : public d::CarefulWriteSink {
 public:
  FailAfterAllocation(d::CarefulWriteSink& inner, u64 map_end) : inner_(inner), map_end_(map_end) {}
  d::BoundedIoResult WriteAt(u64 offset, std::span<const byte> bytes) override {
    if (!failed && offset >= map_end_) {
      failed = true;
      d::BoundedIoResult r;
      r.error = d::BoundedIoError::native_failure;
      r.native_error = EIO;
      r.native_attempted = true;
      return r;
    }
    return inner_.WriteAt(offset, bytes);
  }
  d::BoundedIoResult ReadAt(u64 offset, std::span<byte> bytes) override { return inner_.ReadAt(offset, bytes); }
  d::BoundedIoResult Sync() override { return inner_.Sync(); }
  bool forced_ordered() const noexcept override { return inner_.forced_ordered(); }
  bool failed = false;

 private:
  d::CarefulWriteSink& inner_;
  u64 map_end_;
};
std::vector<db::CatalogNewPageIdentity> PageIds(unsigned k) {
  return {{V7(kT + 700000 + k, 0xe30000 + k), V7(kT + 700000 + k, 0xe40000 + k)}};
}
// Named objects inserted by one transaction until one needs a new base leaf
// of the names relation; then, on a copy of the file before that insert, a
// crash after its allocation: the page is allocated and nothing reaches it
// (wasted space, Q17 = 1), the database verifies, and the insert then
// succeeds on another new page.
void NameGrowth(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "growth.db");
  Session s(b, path);
  Begin(s, 2);
  std::optional<std::pair<std::vector<byte>, db::CatalogInsertRequest>> grew;
  unsigned k = 1;
  for (; k <= 60 && !grew; ++k) {
    const auto file = ReadAll(path);
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 40 + k, 2, "w3b_grow_" + std::to_string(k));
    auto r = Request(2, named.Rows());
    r.new_pages = PageIds(k);
    // Stop when the writer refuses (an index leaf full first: splits are the
    // next step), found by a trial on a copy.
    if (!InsertOracle(b, file, r)) {
      const auto trial = b.Copy(file, "growth-trial.db");
      Session t2(b, trial, false);
      const auto tried = db::WriteCatalogInsertVersions(t2.sink(), GrowingBinding(b), r);
      fs::remove(trial);
      if (!tried.ok()) {
        std::cout << b.name << " growth page_size=" << b.size << " stops at named object " << k << ": "
                  << db::CatalogVersionWriteErrorName(tried.error) << '\n';
        break;
      }
    }
    const auto pages = ExpectGrowingInsert(b, s.sink(), path, r, "named object " + std::to_string(k));
    // The growth this covers is a new BASE leaf (page type 6), alone. An insert
    // that only splits a name-tree leaf (0x202) comes first at some page sizes
    // (since SB_CATALOG_NO_TEXT_IDENTIFIERS changed the name rows' sizes), so it
    // is inserted and checked like the rest, and the loop goes on.
    if (pages.size() == 1) {
      const auto now = ReadAll(path);
      const auto allocated = AllocationOf(b, now, pages.front());
      if (allocated.record && allocated.record->page_type == 6) grew = std::pair{file, r};
    }
  }
  if (!grew) {
    std::cout << b.name << " growth page_size=" << b.size << " not reached after " << (k - 1)
              << " named objects (the names relation has room, or an index leaf filled first)\n";
    Rollback(*s.w, s.sink(), 2);
    fs::remove(path);
    return;
  }
  const unsigned inserted = k - 1;
  Commit(*s.w, s.sink(), 2);
  ExpectNamedVerification(b, path, definitions + inserted, "named objects past a new base leaf, committed");
  // The crash, on the file as it was before the insert that grew.
  const auto crash = b.Copy(grew->first, "growth-crash.db");
  {
    Session c(b, crash, false);  // transaction 2 still in progress, as it was
    FailAfterAllocation sink(c.sink(), (b.receipt.map_pages + 1) * u64{b.size});
    const auto result = db::WriteCatalogInsertVersions(sink, GrowingBinding(b), grew->second);
    Check(sink.failed && !result.ok() && result.error == CE::write_failed && result.attempted &&
              result.allocated_pages.size() == 1,
          "a failure after the allocation fails the insert, its page allocated");
    const auto after = ReadAll(crash);
    for (u64 p = b.receipt.map_pages + 1; p * b.size < after.size(); ++p)
      Check(std::equal(after.begin() + static_cast<std::ptrdiff_t>(p * b.size), after.begin() + static_cast<std::ptrdiff_t>((p + 1) * b.size),
                       grew->first.begin() + static_cast<std::ptrdiff_t>(p * b.size)),
            "nothing but the allocation map was written");
    const auto record = AllocationOf(b, after, result.allocated_pages.front());
    Check(record.record && record.record->page_type == 6 && record.record->creator_transaction_uuid.is_nil(),
          "the page is allocated to the system, and nothing reaches it");
  }
  // In the copy the earlier named objects' transaction is still in progress,
  // so nothing of them is committed: the database is as created, plus a
  // wasted page.
  ExpectNamedVerification(b, crash, definitions, "a crash after an allocation (the page is wasted space)", true);
  {
    Session c(b, crash, false);
    auto again = grew->second;
    again.new_pages = PageIds(99);
    const auto pages = ExpectGrowingInsert(b, c.sink(), crash, again, "the insert after the crash");
    Check(pages.size() == 1, "the insert after the crash takes another new page");
  }
  fs::remove(crash);
  fs::remove(path);
  std::cout << b.name << " growth page_size=" << b.size << " new base leaf after " << inserted << " named objects\n";
}

// SB_CATALOG_LEAF_SPLIT (W3b-3a-2): named objects in one scope, by one
// transaction, until the authoritative name tree splits a leaf in the middle
// of the tree (the names sort together, among other names): the old leaf,
// the new leaf, the old right neighbour and the parent all change. Every
// insert is checked byte for byte; committed, the database verifies (names,
// paths, the trees' cells against their rows, allocation coverage).
void NameSplits(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "name-splits.db");
  Session s(b, path);
  Begin(s, 2);
  const auto zero = d::DecodeFilespacePageZero(b.bytes.data(), b.size);
  Check(zero.ok(), "page zero decodes");
  Uuid authoritative{};
  for (const auto& r : zero.record->roots)
    if (r.kind == 22) authoritative = r.object_uuid;
  std::optional<u64> mid;
  unsigned k = 1, added = 0;
  for (; k <= 200 && !mid; ++k) {
    const auto file = ReadAll(path);
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 300 + k, 2, "w3b_split_" + std::to_string(1000 + k));
    auto r = Request(2, named.Rows());
    for (unsigned i = 0; i < 6; ++i)
      r.new_pages.push_back({V7(kT + 900000 + 10 * k + i, 0xe90000 + k), V7(kT + 900000 + 10 * k + i, 0xea0000 + k)});
    if (!InsertOracle(b, file, r)) {
      const auto trial = b.Copy(file, "name-splits-trial.db");
      Session t2(b, trial, false);
      const auto tried = db::WriteCatalogInsertVersions(t2.sink(), GrowingBinding(b), r);
      fs::remove(trial);
      if (!tried.ok()) {
        std::cout << b.name << " name splits page_size=" << b.size << " stop at named object " << k << ": "
                  << db::CatalogVersionWriteErrorName(tried.error) << '\n';
        break;
      }
    }
    const auto pages = ExpectGrowingInsert(b, s.sink(), path, r, "named object " + std::to_string(k));
    added += static_cast<unsigned>(pages.size());
    const auto now = ReadAll(path);
    for (const auto page : pages) {
      if (TypeAt(b, now, page) != 0x202) continue;
      const auto tree_page = pg::DecodeNativeBtreePage(PageOf(now, page, b.size));
      if (tree_page.ok() && tree_page.page->dependencies.index_uuid == authoritative && tree_page.page->right) mid = page;
    }
  }
  if (!mid) {
    std::cout << b.name << " name splits page_size=" << b.size << " no mid-tree split after " << (k - 1) << " named objects\n";
    Rollback(*s.w, s.sink(), 2);
    fs::remove(path);
    return;
  }
  {
    // The neighbour's left link names the new leaf.
    const auto now = ReadAll(path);
    const auto page = pg::DecodeNativeBtreePage(PageOf(now, *mid, b.size));
    const auto next = pg::DecodeNativeBtreePage(PageOf(now, page.page->right->page_number, b.size));
    Check(next.ok() && next.page->left && next.page->left->page_number == *mid,
          "the old right neighbour's left link names the new leaf");
  }
  Commit(*s.w, s.sink(), 2);
  ExpectNamedVerification(b, path, definitions + (k - 1), "named objects past a mid-tree split, committed");
  fs::remove(path);
  std::cout << b.name << " name splits page_size=" << b.size << " mid-tree split after " << (k - 1)
            << " named objects, " << added << " pages added\n";
}

// The name tree of page-zero root `kind` (22 authoritative, 23 UUID-to-name)
// read whole by the production device reader
// (ReadNativeBtreeTreeFromOpenDevices): its leaf cells, or nullopt when it does
// not read.
std::optional<std::size_t> ProductionNameTreeCells(const Db& b, std::span<const byte> file, u16 kind) {
  const auto zero = d::DecodeFilespacePageZero(file.data(), b.size);
  Check(zero.ok(), "page zero decodes");
  u64 root_page = 0;
  for (const auto& r : zero.record->roots)
    if (r.kind == kind) root_page = r.page_number;
  const auto root = pg::DecodeNativeBtreePage(PageOf(file, root_page, b.size));
  Check(root.ok(), "the name tree root decodes");
  const auto path = b.Copy(std::vector<byte>(file.begin(), file.end()), "name-tree-read.db");
  std::optional<std::size_t> out;
  {
    d::FileDevice device;
    Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "the tree reader's read-only open");
    const std::vector<d::NativeFilespaceDevice> devices{{b.primary, b.profile, &device}};
    const d::NativePageReference ref{b.primary, root_page, root.page->header.page_generation, b.profile};
    const auto read = pg::ReadNativeBtreeTreeFromOpenDevices(b.database, devices, ref, root.page->dependencies, file.size());
    if (read.ok()) {
      std::size_t cells = 0;
      for (const auto leaf : read.leaves) cells += read.pages[leaf].page->cells.size();
      out = cells;
    } else {
      std::cerr << "name tree " << kind << " device read error=" << int(read.error) << '\n';
    }
    Check(device.Close().ok(), "close the tree reader's open");
  }
  fs::remove(path);
  return out;
}
u64 NameRootPage(const Db& b, std::span<const byte> file, u16 kind) {
  const auto zero = d::DecodeFilespacePageZero(file.data(), b.size);
  Check(zero.ok(), "page zero decodes");
  for (const auto& r : zero.record->roots)
    if (r.kind == kind) return r.page_number;
  Check(false, "the name tree root");
  return 0;
}
// The highest level of a page of the tree that a sibling link names and no
// parent lists (a split left unfinished), or -1.
int UnlistedLevel(const Db& b, std::span<const byte> file, u64 root_page) {
  const auto tree = ReadTreeAt(b, file, root_page);
  std::set<u64> listed;
  for (const auto& p : tree.pages) listed.insert(p.number);
  int level = -1;
  for (const auto& p : tree.pages)
    if (p.page.right && !listed.contains(p.page.right->page_number)) {
      const auto lag = pg::DecodeNativeBtreePage(PageOf(file, p.page.right->page_number, b.size));
      Check(lag.ok(), "an unlisted page decodes");
      level = std::max<int>(level, lag.page->tree_level);
    }
  return level;
}
// SB_CATALOG_SPLIT_LAG over branch splits (owner Q45 = 1, Q46 = 1): the
// long-named insert that split a branch, again on copies of the file before
// it, by a fresh transaction, failing each of its writes in turn (a crash at
// that point). Every time both name trees still read through the production
// device reader with their cells unchanged or the new one added; the failed
// transaction rolls back; the next long-named insert, by another transaction,
// completes any unfinished split (refusing nothing), commits, and then no
// page of either tree is reachable by a sibling link only, and both trees
// read with one cell more. Returns the highest level a failure point left
// unlisted (-1 for none).
int NameBranchCrashes(const Db& b, const NameTemplate& t, u16 role, std::span<const byte> pre, unsigned k, std::size_t length,
                      const char* what) {
  unsigned failures = 0, unfinished = 0;
  int highest = -1;
  const auto cells = [&](std::span<const byte> f) {
    return std::pair{ProductionNameTreeCells(b, f, 22), ProductionNameTreeCells(b, f, 23)};
  };
  const auto before = cells(pre);
  Check(before.first && before.second, std::string(what) + ": both name trees read before the insert");
  for (u32 w = 1; w <= 64; ++w) {
    const auto copy = b.Copy(std::vector<byte>(pre.begin(), pre.end()), "name-branch-crash.db");
    Session c(b, copy);  // restart: transaction 2 was in progress, now dead
    Begin(c, 3);
    auto file = ReadAll(copy);
    std::string text = "w3b_branch_" + std::to_string(10000 + k) + "_";
    text.resize(length, 'x');
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 2000 + k, 3, text);
    auto r = Request(3, named.Rows());
    for (unsigned i = 0; i < 8; ++i)
      r.new_pages.push_back({V7(kT + 970000 + 10 * w + i, 0xf10000 + w), V7(kT + 970000 + 10 * w + i, 0xf20000 + w)});
    KindFaultSink sink(c.sink(), KindFaultSink::Kind::write, w);
    const auto result = db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
    if (result.ok()) {
      fs::remove(copy);
      break;
    }
    ++failures;
    Rollback(*c.w, c.sink(), 3);
    const auto crashed = ReadAll(copy);
    const int lag = std::max(UnlistedLevel(b, crashed, NameRootPage(b, crashed, 22)), UnlistedLevel(b, crashed, NameRootPage(b, crashed, 23)));
    if (lag >= 0) ++unfinished;
    highest = std::max(highest, lag);
    const auto seen = cells(crashed);
    Check(seen.first && seen.second, std::string(what) + ": after a failed write " + std::to_string(w) + " both name trees read");
    const std::size_t per = *before.second / *before.first;
    Check(*seen.first >= *before.first && *seen.first <= *before.first + 1 && *seen.second >= *before.second &&
              *seen.second <= *before.second + per,
          std::string(what) + ": after a failed write " + std::to_string(w) + " the trees hold their cells, the new one at most");
    Begin(c, 4);
    file = ReadAll(copy);
    std::string next_text = "w3b_branch_next_" + std::to_string(10000 + k) + "_";
    next_text.resize(length, 'y');
    const auto next_named = NamedAfter(b, file, t, role, After(b, file, role), 2600 + k, 4, next_text);
    auto next = Request(4, next_named.Rows());
    for (unsigned i = 0; i < 8; ++i)
      next.new_pages.push_back({V7(kT + 990000 + 10 * w + i, 0xf30000 + w), V7(kT + 990000 + 10 * w + i, 0xf40000 + w)});
    const auto completed = db::WriteCatalogInsertVersions(c.sink(), GrowingBinding(b), next);
    if (!completed.ok()) std::cerr << what << " after failed write " << w << ": " << db::CatalogVersionWriteErrorName(completed.error) << '\n';
    Check(completed.ok(), std::string(what) + ": the next insert succeeds after failed write " + std::to_string(w));
    Commit(*c.w, c.sink(), 4);
    const auto after = ReadAll(copy);
    Check(UnlistedLevel(b, after, NameRootPage(b, after, 22)) < 0 && UnlistedLevel(b, after, NameRootPage(b, after, 23)) < 0,
          std::string(what) + ": the next write completes an unfinished split");
    const auto now = cells(after);
    Check(now.first && now.second && *now.first >= *before.first + 1 && *now.second >= *before.second + per,
          std::string(what) + ": after the next insert both trees read with its cell");
    fs::remove(copy);
  }
  Check(failures > 0 && unfinished > 0, std::string(what) + ": some failure points leave a split unfinished, and they are survived");
  std::cout << b.name << " " << what << " crashes page_size=" << b.size << " failure points " << failures << ", " << unfinished
            << " leaving a split unfinished, highest unlisted level " << highest << '\n';
  return highest;
}

// SB_CATALOG_BRANCH_SPLIT (W3b-3b-2, owner Q46 = 1): named objects whose
// names are as long as the identifier profile admits (up to 240 bytes), so
// the authoritative name tree's separators are long and its branches fill:
// inserted by one transaction until a branch below the root splits (after the
// root branch itself has split, keeping its page one level higher), or 600
// objects. Every insert is checked byte for byte; committed, the database
// verifies.
void NameBranchSplits(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "name-branches.db");
  Session s(b, path);
  Begin(s, 2);
  const auto zero = d::DecodeFilespacePageZero(b.bytes.data(), b.size);
  Check(zero.ok(), "page zero decodes");
  u64 root_page = 0;
  Uuid authoritative{};
  for (const auto& r : zero.record->roots)
    if (r.kind == 22) {
      authoritative = r.object_uuid;
      root_page = r.page_number;
    }
  const std::size_t limit = t.identifier.maximum_identifier_length ? t.identifier.maximum_identifier_length : 240;
  const std::size_t length = std::min<std::size_t>(limit, 240);
  bool root_grew = false, branch_split = false;
  unsigned k = 1, grew_at = 0;
  const auto root_level = [&](std::span<const byte> file) {
    return pg::DecodeNativeBtreePage(PageOf(file, root_page, b.size)).page->tree_level;
  };
  const auto initial_level = root_level(b.bytes);
  std::cout << b.name << " branch splits page_size=" << b.size << " pages=" << b.bytes.size() / b.size
            << " identifier profile maximum length " << t.identifier.maximum_identifier_length << '\n';
  std::vector<byte> pre_root, pre_branch;
  unsigned root_k = 0, branch_k = 0;
  const auto cells_before = std::pair{ProductionNameTreeCells(b, b.bytes, 22), ProductionNameTreeCells(b, b.bytes, 23)};
  Check(cells_before.first && cells_before.second, "both name trees read through the production device reader");
  for (; k <= 2000 && !branch_split; ++k) {
    const auto file = ReadAll(path);
    std::string text = "w3b_branch_" + std::to_string(10000 + k) + "_";
    text.resize(length, 'x');
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 2000 + k, 2, text);
    auto r = Request(2, named.Rows());
    for (unsigned i = 0; i < 8; ++i)
      r.new_pages.push_back({V7(kT + 950000 + 10 * k + i, 0xef0000 + k), V7(kT + 950000 + 10 * k + i, 0xf00000 + k)});
    if (!InsertOracle(b, file, r)) {
      const auto trial = b.Copy(file, "name-branches-trial.db");
      Session t2(b, trial, false);
      const auto tried = db::WriteCatalogInsertVersions(t2.sink(), GrowingBinding(b), r);
      fs::remove(trial);
      if (!tried.ok()) {
        std::cout << b.name << " branch splits page_size=" << b.size << " stop at named object " << k << ": "
                  << db::CatalogVersionWriteErrorName(tried.error) << '\n';
        break;
      }
    }
    const auto pages = ExpectGrowingInsert(b, s.sink(), path, r, "long-named object " + std::to_string(k));
    const auto now = ReadAll(path);
    const bool grew = root_level(now) > root_level(file);
    unsigned branches = 0;
    for (const auto page : pages)
      if (TypeAt(b, now, page) == 0x201 &&
          pg::DecodeNativeBtreePage(PageOf(now, page, b.size)).page->dependencies.index_uuid == authoritative)
        ++branches;
    // A root-branch split adds two branches and a level; a branch split below
    // the root adds one branch and no level.
    if (grew && root_level(now) >= 2 && !root_grew) {
      root_grew = true;
      grew_at = k;
      Check(branches == 2, "a root-branch split adds two branches under the kept root");
      pre_root = file;
      root_k = k;
    }
    if (branches && !grew && !branch_split) {
      branch_split = true;
      pre_branch = file;
      branch_k = k;
    }
  }
  std::cout << b.name << " branch splits page_size=" << b.size << " name length " << length << ": root "
            << (root_grew ? "grew to level 2 at named object " + std::to_string(grew_at) : std::string("did not grow"))
            << ", branch split below the root " << (branch_split ? "yes" : "no") << " after " << (k - 1) << " named objects\n";
  Check(root_grew && branch_split, "long names split the root branch and a branch below it");
  Commit(*s.w, s.sink(), 2);
  const auto final_file = ReadAll(path);
  const auto cells_after = std::pair{ProductionNameTreeCells(b, final_file, 22), ProductionNameTreeCells(b, final_file, 23)};
  if (cells_after.first && cells_after.second)
    std::cout << b.name << " name tree cells before " << *cells_before.first << "/" << *cells_before.second << " after "
              << *cells_after.first << "/" << *cells_after.second << " for " << (k - 1) << " named objects\n";
  // The UUID-to-name tree holds two cells per name entry (2,126 for the
  // create's 1,063 at 8 KiB), the authoritative tree one.
  const std::size_t per = *cells_before.second / *cells_before.first;
  Check(per * *cells_before.first == *cells_before.second, "the UUID-to-name tree holds a whole number of cells per entry");
  Check(cells_after.first && cells_after.second && *cells_after.first == *cells_before.first + (k - 1) &&
            *cells_after.second == *cells_before.second + per * (k - 1),
        "committed, both name trees read through the production device reader with each named object's cells");
  ExpectNamedVerification(b, path, definitions + (k - 1), "long-named objects past branch splits, committed", false, true);
  NameBranchCrashes(b, t, role, pre_root, root_k, length, "root-branch split");
  const auto lag = NameBranchCrashes(b, t, role, pre_branch, branch_k, length, "branch split");
  Check(lag >= 1, "some failure point of a branch split leaves a branch unlisted, and it is survived");
  fs::remove(path);
}

// ---- Name successors (N4a) ---------------------------------------------------------------
// The file after a name row's successor: the row placed as the version
// writer's oracle places it, its envelope bound there; the base version's
// cells stamped with the transaction in each tree that holds one; a version
// that is not a drop given its own cells.
struct NameSuccessorExpected {
  std::vector<byte> file;
  std::size_t writes = 0;
};
// `new_page`: where the writer put a new base leaf when no leaf had room
// (SB_CATALOG_NEW_PAGES); the leaf is then a sibling's header and body
// identity with this page's number, the request's first page UUID and
// generation 1, holding the version, and the version gets its own cell in the
// names relation's row-UUID tree.
std::optional<NameSuccessorExpected> NameSuccessorOracle(const Db& b, std::span<const byte> before,
                                                         const db::CatalogVersionRequest& r,
                                                         std::optional<u64> new_page = std::nullopt) {
  RoleScope scope(b, 2);
  const auto rel = ReadRelation(b, before);
  const auto versions = rel.Versions(r.row_uuid);
  Check(!versions.empty(), "the name row exists");
  const u64 newest = rel.Row(versions.front()).row_version;
  const auto base = rel.Find(r.predecessor_version_uuid);
  const auto finished = [&](const LeafSeen& l) {
    auto m = r.metadata;
    u32 slot = 0;
    for (const auto& x : l.leaf.body.rows) slot = std::max(slot, x.stable_slot_id);
    const c::CatalogNameVersionBinding at{
        {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
        {UuidKind::row, r.row_uuid}, {UuidKind::row, r.version_uuid}, m.record.header.object_uuid,
        {UuidKind::transaction, r.transaction_uuid}, l.page, slot + 1, l.leaf.header.page_generation, newest + 1,
        r.transaction_order, m.catalog_generation};
    const auto envelope = c::EncodeCatalogNameEnvelope({at, *r.name});
    Check(envelope.ok(), "the expected successor envelope encodes");
    m.record.payload.assign(envelope.bytes.begin(), envelope.bytes.end());
    return m;
  };
  std::optional<u64> target;
  if (rel.At(base.first).leaf.body.free_space_bytes >= RowBytes(finished(rel.At(base.first)))) target = base.first;
  for (const auto& l : rel.leaves)
    if (!target && l.page != base.first && l.leaf.body.free_space_bytes >= RowBytes(finished(l))) target = l.page;
  std::optional<LeafSeen> fresh;
  if (!target && new_page) {
    LeafSeen l;
    l.page = *new_page;
    l.leaf.header = rel.leaves.front().leaf.header;
    l.leaf.header.page_uuid = r.new_pages.front().page_uuid;
    l.leaf.header.page_number = *new_page;
    l.leaf.header.page_generation = 1;
    l.leaf.body = rel.leaves.front().leaf.body;
    l.leaf.body.rows.clear();
    l.leaf.body.page_number = *new_page;
    l.leaf.body.page_generation = 1;
    fresh = std::move(l);
    target = *new_page;
  }
  if (!target) return std::nullopt;
  const auto& target_leaf = fresh ? *fresh : rel.At(*target);
  auto link = rel.At(base.first).leaf;
  link.body.rows[base.second].next_version_uuid = r.version_uuid;
  link.body.rows[base.second].next_row_version = newest + 1;
  auto into = *target == base.first ? link : target_leaf.leaf;
  const auto final_metadata = finished(target_leaf);
  pg::RowDataRecord row;
  row.row_uuid = {UuidKind::row, r.row_uuid};
  row.version_uuid = r.version_uuid;
  row.previous_version_uuid = r.predecessor_version_uuid;
  row.transaction_uuid = {UuidKind::transaction, r.transaction_uuid};
  row.local_transaction_id = r.transaction_order;
  row.row_version = newest + 1;
  row.previous_row_version = rel.Row(base).row_version;
  row.storage_generation = into.header.page_generation;
  u32 slot = 0;
  for (const auto& x : into.body.rows) slot = std::max(slot, x.stable_slot_id);
  row.stable_slot_id = slot + 1;
  const auto encoded = c::EncodeCatalogMetadataVersion(final_metadata);
  Check(encoded.ok(), "the successor metadata encodes");
  scratchbird::core::datatypes::DatatypeBinaryValue cell;
  cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
  cell.payload = encoded.bytes;
  row.cells = {{1, std::move(cell), {}}};
  into.body.rows.push_back(std::move(row));
  NameSuccessorExpected out;
  out.file.assign(before.begin(), before.end());
  const auto put_leaf = [&](const db::NativeCatalogLeafPage& page) {
    const auto image = db::EncodeNativeCatalogLeaf(page);
    Check(image.ok(), "the expected leaf encodes");
    std::copy(image.bytes.begin(), image.bytes.end(), out.file.begin() + static_cast<std::ptrdiff_t>(page.header.page_number * b.size));
    ++out.writes;
  };
  put_leaf(into);
  if (*target != base.first) put_leaf(link);
  // A version on a new leaf: its own cell in the row-UUID tree, after the
  // leaves (the tree read here is the file's as it stands).
  std::size_t index_writes = 0;
  if (fresh) {
    auto tree = ReadTree(b, out.file);
    const auto key = KeyOf(r.row_uuid, r.version_uuid);
    auto& leaf = tree.pages[LeafFor(tree, key)];
    std::size_t at = 0;
    while (at < leaf.page.cells.size() && KeyOrder(leaf.page.cells[at].key, key) < 0) ++at;
    pg::NativeBtreeCell entry;
    entry.key = key;
    entry.base_page = d::NativePageReference{b.primary, *new_page, 1, b.profile};
    entry.creator_local_transaction_id = r.transaction_order;
    leaf.page.cells.insert(leaf.page.cells.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry));
    const auto image = pg::EncodeNativeBtreePage(leaf.page);
    if (!image.ok()) return std::nullopt;
    std::copy(image.bytes.begin(), image.bytes.end(), out.file.begin() + static_cast<std::ptrdiff_t>(leaf.number * b.size));
    index_writes = 1;
  }
  // The trees.
  const auto zero = d::DecodeFilespacePageZero(out.file.data(), b.size);
  Check(zero.ok(), "page zero decodes");
  const auto root_of = [&](u16 kind) -> std::optional<u64> {
    for (const auto& root : zero.record->roots) if (root.kind == kind) return root.page_number;
    return std::nullopt;
  };
  const auto first = root_of(22), second = root_of(23);
  Check(first.has_value() && second.has_value(), "the production database has both name trees");
  std::set<u64> tree_pages_changed;
  const auto put_tree = [&](u64 number, const pg::NativeBtreePage& page) {
    const auto image = pg::EncodeNativeBtreePage(page);
    Check(image.ok(), "the expected tree page encodes");
    std::copy(image.bytes.begin(), image.bytes.end(), out.file.begin() + static_cast<std::ptrdiff_t>(number * b.size));
    tree_pages_changed.insert(number);
  };
  for (unsigned t = 0; t < 2; ++t) {
    if (t == 0 && !std::holds_alternative<c::CatalogNameEntry>(*r.name)) continue;
    auto tree = ReadTreeAt(b, out.file, t ? *second : *first);
    bool stamped = false;
    for (const auto i : tree.leaves) {
      auto& page = tree.pages[i];
      for (auto& cell : page.page.cells)
        if (cell.key.version_uuid == r.predecessor_version_uuid && cell.key.row_uuid == r.row_uuid) {
          cell.superseding_local_transaction_id = r.transaction_order;
          put_tree(page.number, page.page);
          stamped = true;
          break;
        }
      if (stamped) break;
    }
    Check(stamped, "the base version's cell is found to stamp");
  }
  if (!r.metadata.record.header.deleted) {
    std::deque<c::CatalogNamePayload> keep;
    c::CatalogNameIndex names;
    const auto stored = NamesOf(b, out.file);
    for (const auto& v : stored.vectors) names.Add(c::BorrowCatalogNamePayload(keep.emplace_back(v)));
    for (const auto& e : stored.entries) names.Add(c::BorrowCatalogNamePayload(keep.emplace_back(e)));
    names.Add(c::BorrowCatalogNamePayload(*r.name));
    struct Planned { unsigned tree; std::vector<byte> key, payload; };
    std::vector<Planned> cells;
    const auto view = c::BorrowCatalogNamePayload(*r.name);
    if (const auto* v = std::get_if<c::CatalogNameVectorView>(&view)) {
      const auto kind = std::optional{v->object_kind};
      Check(c::IsKnownCatalogRecordKind(*kind), "the vector's kind is a record kind");
      cells.push_back({1, c::CatalogNameUuidTreeVectorKey(v->object_uuid.value, *kind), c::CatalogNameUuidTreeVectorPayload(*v)});
    } else {
      const auto& e = std::get<c::CatalogNameEntryView>(view);
      const auto kind = std::optional{e.object_kind};
      Check(c::IsKnownCatalogRecordKind(*kind), "the entry's kind is a record kind");
      const auto path = c::ResolveCatalogNamePath(e, names);
      Check(path.ok(), "the successor's path resolves for the oracle");
      std::vector<std::string_view> components;
      for (const auto& component : path.components) components.push_back(component.display_name);
      cells.push_back({0, c::CatalogNameAuthoritativeTreeKey(e, *kind), c::CatalogNameAuthoritativeTreePayload(e)});
      cells.push_back({1, c::CatalogNameUuidTreeEntryKey(e, *kind), c::CatalogNameUuidTreeEntryPayload(e.name_entry_uuid.value, components)});
    }
    for (auto& planned : cells) {
      auto tree = ReadTreeAt(b, out.file, planned.tree ? *second : *first);
      pg::NativeBtreeKey key;
      key.encoded_key = std::move(planned.key);
      key.row_uuid = r.row_uuid;
      key.version_uuid = r.version_uuid;
      auto& leaf = tree.pages[LeafFor(tree, key)];
      std::size_t at = 0;
      while (at < leaf.page.cells.size() && KeyOrder(leaf.page.cells[at].key, key) < 0) ++at;
      pg::NativeBtreeCell entry;
      entry.key = key;
      entry.base_page = d::NativePageReference{b.primary, *target, into.header.page_generation, b.profile};
      entry.creator_local_transaction_id = r.transaction_order;
      entry.payload = std::move(planned.payload);
      leaf.page.cells.insert(leaf.page.cells.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry));
      const auto image = pg::EncodeNativeBtreePage(leaf.page);
      if (!image.ok()) return std::nullopt;
      put_tree(leaf.number, leaf.page);
    }
  }
  out.writes += tree_pages_changed.size() + index_writes;
  return out;
}
// False when no leaf of the names relation has room for the version: the
// writer then refuses relation_full with no write (new pages: W3b-3).
// SB_CATALOG_NEW_PAGES (W3b-3a): when no leaf has room the version goes on a
// new base leaf; the oracle is then given the page the writer allocated, whose
// record is checked on its own (the system's: no creator; the lowest free
// slot; type, owner, identities, generation 1), and the map pages are taken
// from the file once their one record is.
bool ExpectNameSuccessor(const Db& b, Session& s, const std::string& path, const db::CatalogVersionRequest& r,
                         std::string_view label) {
  const auto before = ReadAll(path);
  const auto in_place = NameSuccessorOracle(b, before, r);
  const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
  if (!result.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(result.error) << '\n';
  Check(result.ok(), std::string(label) + ": the successor is written");
  const auto after = ReadAll(path);
  if (in_place) {
    Check(result.allocated_pages.empty(), std::string(label) + ": no page is added while a leaf has room");
    Check(result.writes == in_place->writes, std::string(label) + ": one write per changed page");
    Check(after == in_place->file, std::string(label) + ": the file is the oracle's");
    return true;
  }
  Check(result.allocated_pages.size() == 1, std::string(label) + ": one new base leaf");
  const auto page = result.allocated_pages.front();
  auto expected = NameSuccessorOracle(b, before, r, page);
  Check(expected.has_value(), std::string(label) + ": the oracle places the version on the new leaf");
  const auto was = AllocationOf(b, before, page);
  const auto now = AllocationOf(b, after, page);
  Check(!was.record && was.lower_all_allocated, std::string(label) + ": the lowest free slot");
  RoleScope scope(b, 2);
  Check(now.record && now.record->page_type == 6 && now.record->owner_uuid == b.Relation().relation_uuid &&
            now.record->page_uuid == r.new_pages.front().page_uuid &&
            now.record->allocation_uuid == r.new_pages.front().allocation_uuid &&
            now.record->creator_transaction_uuid.is_nil() && !now.record->creator_local_transaction_id &&
            now.record->creator_operation_uuid.is_nil() && now.record->page_generation == 1,
        std::string(label) + ": the system's allocation record for the new leaf");
  for (u64 m = 1; m <= b.receipt.map_pages; ++m)
    std::copy(after.begin() + static_cast<std::ptrdiff_t>(m * b.size), after.begin() + static_cast<std::ptrdiff_t>((m + 1) * b.size),
              expected->file.begin() + static_cast<std::ptrdiff_t>(m * b.size));
  Check(result.writes == expected->writes + 1 || result.writes == expected->writes + 2,
        std::string(label) + ": the allocation's one or two writes, then one write per changed page");
  Check(after == expected->file, std::string(label) + ": the file is the oracle's");
  std::cout << b.name << " name successor page_size=" << b.size << " new base leaf " << page << ": " << label << '\n';
  return true;
}
// A successor request for a name row the test inserted: the insert's
// metadata advanced by one version under transaction `n`.
db::CatalogVersionRequest NameSuccessor(const db::CatalogInsertRow& inserted, const Uuid& predecessor, u64 n,
                                        const Uuid& version, c::CatalogNamePayload name, bool drop) {
  db::CatalogVersionRequest r;
  r.role = 2;
  r.transaction_order = n;
  r.transaction_uuid = TxId(n);
  r.row_uuid = inserted.row_uuid;
  r.predecessor_version_uuid = predecessor;
  r.version_uuid = version;
  r.metadata = inserted.metadata;
  r.metadata.definition_version += 1;
  r.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  r.metadata.creator_local_transaction_id = n;
  if (drop) {
    r.metadata.record.header.deleted = true;
    r.metadata.lifecycle = c::CatalogObjectLifecycle::dropped;
    r.metadata.status = c::CatalogObjectStatus::retired;
    r.metadata.retired_transaction_uuid = {UuidKind::transaction, TxId(n)};
  }
  r.name = std::move(name);
  // One page identity, should the version need a new base leaf.
  r.new_pages = {{V7(Ms(version), 0xe10000 + version.bytes[15]), V7(Ms(version), 0xe20000 + version.bytes[15])}};
  return r;
}
// A written successor as the base of the next one.
db::CatalogInsertRow AsRow(const db::CatalogVersionRequest& r) {
  return db::CatalogInsertRow{2, r.row_uuid, r.version_uuid, r.metadata, r.name};
}
// ---- Name cascade (N4b) ------------------------------------------------------------------
// Every version of every name entry row: where the cascade checks look.
struct EntryVersion {
  Uuid row, version;
  u64 row_version = 0, order = 0;
  bool deleted = false;
  c::CatalogNameEntry entry;
};
std::vector<EntryVersion> EntryVersionsOf(const Db& b, std::span<const byte> file) {
  RoleScope scope(b, 2);
  const auto rel = ReadRelation(b, file);
  std::vector<EntryVersion> out;
  for (const auto& l : rel.leaves)
    for (const auto& row : l.leaf.body.rows) {
      const auto& m = l.metadata.at(row.version_uuid);
      const c::CatalogNameVersionBinding at{
          {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
          row.row_uuid, {UuidKind::row, row.version_uuid}, m.record.header.object_uuid, row.transaction_uuid, l.page,
          row.stable_slot_id, row.storage_generation, row.row_version, row.local_transaction_id, m.catalog_generation};
      const std::vector<byte> bytes(reinterpret_cast<const byte*>(m.record.payload.data()),
                                    reinterpret_cast<const byte*>(m.record.payload.data()) + m.record.payload.size());
      const auto decoded = c::DecodeCatalogNameEnvelope(bytes, at);
      Check(decoded.ok(), "every stored name decodes against where it is");
      if (const auto* e = std::get_if<c::CatalogNameEntry>(&decoded.record->payload))
        out.push_back({row.row_uuid.value, row.version_uuid, row.row_version, row.local_transaction_id,
                       m.record.header.deleted, *e});
    }
  return out;
}
// Each entry row's newest version, the versions of transactions in `dead` left out.
std::map<Uuid, EntryVersion> NewestEntries(const Db& b, std::span<const byte> file, const std::set<u64>& dead = {}) {
  std::map<Uuid, EntryVersion> out;
  for (auto& v : EntryVersionsOf(b, file)) {
    if (dead.contains(v.order)) continue;
    const auto at = out.find(v.row);
    if (at == out.end() || at->second.row_version < v.row_version) out.insert_or_assign(v.row, std::move(v));
  }
  return out;
}
bool StartsWith(std::span<const byte> whole, std::span<const byte> prefix) {
  return whole.size() > prefix.size() && std::equal(prefix.begin(), prefix.end(), whole.begin());
}
// SB_NATIVE_NAME_CASCADE (N4b): renaming a scope's name (the template's
// schema, whose name is a component of every path below it) regenerates every
// entry below it, in the same write. Written out here rather than computed by
// the shared path rule: the entries below are those whose stored full path has
// the scope's old full path as a prefix, and each one's new full path is the
// scope's new full path followed by the rest of its old one; its UUID-to-name
// display components change at the scope's position only; nothing else of the
// entry changes. Also: a conflict with another transaction's entry below the
// scope, the identities a cascade needs, a drop that would leave the entries
// below without a path, and failing each write of the cascading rename in turn.
void NameCascade(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "cascade.db");
  Session s(b, path);
  // The schema's name in the template's language, and its row.
  // `dead`: transactions that rolled back, whose versions are not the base.
  const auto scope_entry = [&](std::span<const byte> file, const std::set<u64>& dead = {}) {
    for (const auto& [row, v] : NewestEntries(b, file, dead))
      if (!v.deleted && v.entry.object_uuid.value == t.entry.scope_uuid.value && v.entry.language_uuid.value == t.entry.language_uuid.value &&
          v.entry.default_for_language)
        return v;
    Check(false, "the template's scope has a name in the template's language");
    return EntryVersion{};
  };
  const auto as_row = [&](std::span<const byte> file, const EntryVersion& v) {
    RoleScope a(b, 2);
    const auto rel = ReadRelation(b, file);
    for (const auto& l : rel.leaves)
      for (const auto& row : l.leaf.body.rows)
        if (row.version_uuid == v.version) {
          db::CatalogInsertRow r{2, row.row_uuid.value, row.version_uuid, l.metadata.at(row.version_uuid), c::CatalogNamePayload{v.entry}};
          r.metadata.record.payload.clear();
          return r;
        }
    Check(false, "the scope's name row is found");
    return db::CatalogInsertRow{};
  };
  const auto below = [&](std::span<const byte> file, const std::vector<byte>& prefix) {
    std::vector<EntryVersion> out;
    for (const auto& [row, v] : NewestEntries(b, file))
      if (!v.deleted && v.entry.lifecycle_state == c::CatalogNameLifecycle::active && StartsWith(v.entry.full_path_lookup_key, prefix))
        out.push_back(v);
    return out;
  };
  const auto rename_request = [&](std::span<const byte> file, u64 n, unsigned k, std::size_t identities,
                                  const std::set<u64>& dead = {}) {
    const auto old = scope_entry(file, dead);
    Check(old.entry.identifier_profile_uuid.value == t.entry.identifier_profile_uuid.value,
          "the scope's name keys under the template's identifier profile");
    auto e = Keyed(old.entry, t, "orders_cascaded");
    PathBeside(e, old.entry);
    auto r = NameSuccessor(as_row(file, old), old.version, n, VersionId(k), c::CatalogNamePayload{e}, false);
    // Their own range: VersionId's would meet other requests' identities.
    for (std::size_t i = 0; i < identities; ++i)
      r.cascade_versions.push_back(V7(kT + 1300000 + 1000 * u64{k} + i, 0x700000 + 4096 * k + static_cast<unsigned>(i)));
    for (unsigned i = 0; i < 16; ++i)
      r.new_pages.push_back({V7(kT + 1100000 + 20 * k + i, 0xf50000 + k), V7(kT + 1100000 + 20 * k + i, 0xf60000 + k)});
    return std::pair{r, e};
  };

  // Another transaction's entry below the scope, in progress: the rename
  // cannot cascade it and is a write conflict with that transaction.
  Begin(s, 2);
  {
    const auto file = ReadAll(path);
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 3101, 2, "w3b_below_scope");
    ExpectInsert(b, s, path, Request(2, named.Rows()), "a named object below the scope, in progress");
  }
  Begin(s, 3);
  {
    const auto file = ReadAll(path);
    auto [r, e] = rename_request(file, 3, 9100, 4096);
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    Check(result.error == CE::write_conflict && result.blocker_order == u64{2} && !result.attempted && ReadAll(path) == file,
          "renaming the scope over another transaction's entry below it is a write conflict with it, no write");
  }
  Commit(*s.w, s.sink(), 2);

  // The identities the cascade needs, refused before any write when short.
  auto pre = ReadAll(path);
  const auto old_scope = scope_entry(pre);
  const auto expected = below(pre, old_scope.entry.full_path_lookup_key);
  Check(!expected.empty(), "entries below the scope");
  {
    auto [r, e] = rename_request(pre, 3, 9100, expected.size() - 1);
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    std::cout << b.name << " cascade short: " << db::CatalogVersionWriteErrorName(result.error) << " needed " << result.cascade_needed
              << " expected " << expected.size() << '\n';
    Check(result.error == CE::cascade_versions_exhausted && result.cascade_needed == expected.size() && !result.attempted &&
              result.allocated_pages.empty() && ReadAll(path) == pre,
          "one identity short: cascade_versions_exhausted, the count needed, nothing written");
  }
  // The rename and its cascade, checked against the rule above.
  const auto check_cascade = [&](std::span<const byte> after, const db::CatalogVersionWriteResult& result,
                                 const c::CatalogNameEntry& renamed, const std::string& label) {
    Check(result.cascaded.size() == expected.size(), label + ": one version per entry below the scope");
    const auto versions = EntryVersionsOf(b, after);
    const auto t2 = ReadTreeAt(b, after, NameRootPage(b, after, 23));
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto& old = expected[i];
      Check(i == 0 || expected[i - 1].row < old.row, "entries in row-UUID order");
      Check(result.cascaded[i].first == old.row, label + ": the cascade in row-UUID order");
      const auto found = std::find_if(versions.begin(), versions.end(), [&](const auto& v) { return v.version == result.cascaded[i].second; });
      Check(found != versions.end() && found->row == old.row && found->row_version > old.row_version && !found->deleted,
            label + ": the cascaded version is stored");
      auto want = old.entry;
      want.full_path_lookup_key = renamed.full_path_lookup_key;
      want.full_path_lookup_key.insert(want.full_path_lookup_key.end(),
                                       old.entry.full_path_lookup_key.begin() + static_cast<std::ptrdiff_t>(old_scope.entry.full_path_lookup_key.size()),
                                       old.entry.full_path_lookup_key.end());
      Check(found->entry.full_path_lookup_key == want.full_path_lookup_key &&
                found->entry.path_component_count == old.entry.path_component_count,
            label + ": the full path is the scope's new path then the rest of the old one");
      // Nothing else of the entry changes but the catalog generation.
      auto same = found->entry;
      same.full_path_lookup_key = old.entry.full_path_lookup_key;
      same.catalog_generation_id = old.entry.catalog_generation_id;
      const auto a = c::EncodeCatalogNameEnvelope({{}, c::CatalogNamePayload{same}});
      const auto z = c::EncodeCatalogNameEnvelope({{}, c::CatalogNamePayload{old.entry}});
      Check(a.ok() == z.ok() && a.bytes == z.bytes, label + ": nothing else of the entry changes");
      bool cell = false;
      for (const auto leaf : t2.leaves)
        for (const auto& x : t2.pages[leaf].page.cells) {
          if (x.key.version_uuid != found->version) continue;
          const auto payload = c::DecodeCatalogNameUuidTreeEntryPayload(x.payload);
          Check(payload.has_value() && payload->display_components.size() == old.entry.path_component_count &&
                    payload->display_components[old_scope.entry.path_component_count - 1] == renamed.display_name,
                label + ": the UUID-to-name cell shows the scope's new name at its place");
          cell = true;
        }
      Check(cell, label + ": the cascaded version has its UUID-to-name cell");
    }
  };
  const auto written_pre = pre;
  {
    auto [r, e] = rename_request(pre, 3, 9100, expected.size());
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!result.ok()) std::cerr << "cascade: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "renaming the scope cascades to the entries below it");
    const auto after = ReadAll(path);
    check_cascade(after, result, e, "the cascading rename");
    std::cout << b.name << " name cascade page_size=" << b.size << ": " << expected.size() << " entries below the scope, "
              << result.writes << " writes, " << result.allocated_pages.size() << " new pages\n";
    Commit(*s.w, s.sink(), 3);
    ExpectNamedVerification(b, path, definitions + 1, "a scope rename and its cascade, committed", false, true);
    // Dropping the scope's only name in its language would leave the entries
    // below without a path: refused.
    Begin(s, 4);
    const auto now = ReadAll(path);
    const auto renamed_scope = scope_entry(now);
    auto dropped = renamed_scope.entry;
    dropped.lifecycle_state = c::CatalogNameLifecycle::dropped;
    dropped.dropped_transaction_uuid = {UuidKind::transaction, TxId(4)};
    auto drop = NameSuccessor(as_row(now, renamed_scope), renamed_scope.version, 4, VersionId(9900), c::CatalogNamePayload{dropped}, true);
    drop.cascade_versions = {VersionId(9901)};
    Refused(b, s, path, drop, CE::invalid_successor, "dropping the scope's name, which paths below it need");
    Rollback(*s.w, s.sink(), 4);
  }
  // SB_CATALOG_SPLIT_LAG / careful write: the cascading rename again on copies
  // of the file before it, by a fresh transaction, failing each of its writes
  // in turn. Every time the database verifies (the failed transaction's
  // versions and stamps are a dead transaction's); the rename then succeeds by
  // another transaction, cascading the same entries, and verifies.
  unsigned failures = 0;
  for (u32 w = 1; w <= 256; ++w) {
    const auto copy = b.Copy(written_pre, "cascade-crash.db");
    Session c(b, copy);  // restart: transaction 3 was in progress, now dead
    Begin(c, 4);
    auto [r, e] = rename_request(written_pre, 4, 9200, expected.size());
    KindFaultSink sink(c.sink(), KindFaultSink::Kind::write, w);
    const auto result = db::WriteCatalogSuccessorVersion(sink, GrowingBinding(b), r);
    if (result.ok()) {
      fs::remove(copy);
      break;
    }
    ++failures;
    Check(result.error == CE::write_failed, "a failed write is write_failed");
    Rollback(*c.w, c.sink(), 4);
    if (w <= 3 || w % 8 == 0) ExpectNamedVerification(b, copy, definitions + 1, "after a failed cascade write " + std::to_string(w), false, true);
    Begin(c, 5);
    const auto file = ReadAll(copy);
    auto [next, ne] = rename_request(file, 5, 9600, expected.size(), {4});
    const auto done = db::WriteCatalogSuccessorVersion(c.sink(), GrowingBinding(b), next);
    if (!done.ok()) std::cerr << "after failed cascade write " << w << ": " << db::CatalogVersionWriteErrorName(done.error) << '\n';
    Check(done.ok(), "the rename succeeds after failed cascade write " + std::to_string(w));
    check_cascade(ReadAll(copy), done, ne, "the rename after failed write " + std::to_string(w));
    Commit(*c.w, c.sink(), 5);
    if (w <= 3 || w % 8 == 0) ExpectNamedVerification(b, copy, definitions + 1, "the rename after failed write " + std::to_string(w), false, true);
    fs::remove(copy);
  }
  Check(failures > 0, "the cascading rename has failure points");
  std::cout << b.name << " name cascade crashes page_size=" << b.size << " failure points " << failures << '\n';
  fs::remove(path);
}

// The newest version of the name row whose payload `match` accepts, as a
// request row (payload cleared, the name alongside).
template <class Match>
db::CatalogInsertRow NameRowOf(const Db& b, std::span<const byte> file, Match match, const std::string& what) {
  RoleScope scope(b, 2);
  const auto rel = ReadRelation(b, file);
  std::optional<db::CatalogInsertRow> out;
  u64 newest = 0;
  for (const auto& l : rel.leaves)
    for (const auto& row : l.leaf.body.rows) {
      const auto& m = l.metadata.at(row.version_uuid);
      const c::CatalogNameVersionBinding at{
          {UuidKind::database, l.leaf.header.database_uuid}, {UuidKind::filespace, l.leaf.header.filespace_uuid},
          row.row_uuid, {UuidKind::row, row.version_uuid}, m.record.header.object_uuid, row.transaction_uuid, l.page,
          row.stable_slot_id, row.storage_generation, row.row_version, row.local_transaction_id, m.catalog_generation};
      const std::vector<byte> bytes(reinterpret_cast<const byte*>(m.record.payload.data()),
                                    reinterpret_cast<const byte*>(m.record.payload.data()) + m.record.payload.size());
      const auto decoded = c::DecodeCatalogNameEnvelope(bytes, at);
      Check(decoded.ok(), "every stored name decodes against where it is");
      if (!match(decoded.record->payload) || (out && row.row_version < newest)) continue;
      newest = row.row_version;
      out = db::CatalogInsertRow{2, row.row_uuid.value, row.version_uuid, m, decoded.record->payload};
      out->metadata.record.payload.clear();
    }
  Check(out.has_value(), what + " is found");
  return *out;
}
// A further entry for the object `of` names (by `n`, at row `key`): `text`
// in `language`, as `name_class`, default for its language or not, named in
// the same scope.
// A second language for a test: fr-CA, or de-DE when the template is fr-CA
// (language constants, SB_CATALOG_LANGUAGE_CONSTANTS; was "fr"/"de" text).
Uuid OtherLanguage(const Uuid& template_language) {
  return template_language == c::kCatalogLanguage_fr_CA ? c::kCatalogLanguage_de_DE : c::kCatalogLanguage_fr_CA;
}
db::CatalogInsertRow EntryLike(const db::CatalogInsertRow& of, const NameTemplate& t, const Uuid& key, unsigned k, u64 n,
                               const std::string& text, const Uuid& language, bool default_for_language,
                               c::CatalogNameClass name_class) {
  auto row = of;
  row.row_uuid = key;
  row.version_uuid = V7(Ms(key), 0x740000 + k);
  const scratchbird::core::platform::TypedUuid id{UuidKind::object, V7(Ms(key), 0x750000 + k)};
  row.metadata.record.header.object_uuid = id;
  row.metadata.record.header.row_uuid = {UuidKind::row, key};
  row.metadata.default_name_uuid = id;
  row.metadata.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  row.metadata.creator_local_transaction_id = n;
  row.metadata.definition_version = 1;
  const auto& base = std::get<c::CatalogNameEntry>(*of.name);
  auto e = Keyed(base, t, text);
  e.name_entry_uuid = id;
  e.language_uuid = c::CatalogLanguageIdentity(language);
  e.name_class = name_class;
  e.default_for_object = false;
  e.default_for_language = default_for_language;
  e.created_transaction_uuid = row.metadata.creator_transaction_uuid;
  e.catalog_generation_id = row.metadata.catalog_generation;
  PathBeside(e, base);
  row.name = c::CatalogNamePayload{e};
  Check(c::CatalogNamePayloadMatchesMetadata(*row.name, row.metadata), "the further entry agrees with its metadata");
  return row;
}
// SB_NATIVE_NAME_CASCADE (N4b): the path rule's choices, each moved by a
// change to the schema's names and checked against expectations written out
// here (not computed by the shared path rule):
//  - an entry in a language the schema has no name in (French) runs through
//    the schema's vector default;
//  - the schema's default name changed to an alias (four rows by one
//    transaction: the old entry unmarked, which moves nothing while it is the
//    vector's default; the alias marked default, which moves every English
//    entry below; the vector's default, which moves the French one; the
//    definition's default name);
//  - the old English entry, which no path now uses, dropped: nothing moves.
// Each transaction is committed and the database verifies.
void NameCascadePaths(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "cascade-paths.db");
  Session s(b, path);
  const auto schema = t.entry.scope_uuid.value;
  const auto entry_of = [&](std::span<const byte> file, const Uuid& language, bool default_only) {
    return NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
      const auto* e = std::get_if<c::CatalogNameEntry>(&p);
      return e && e->object_uuid.value == schema && e->language_uuid.value == language && (!default_only || e->default_for_language);
    }, "the schema's entry in the language");
  };
  const auto english = std::get<c::CatalogNameEntry>(*entry_of(b.bytes, t.entry.language_uuid.value, true).name);
  const Uuid other_language = OtherLanguage(t.entry.language_uuid.value);
  // A named object below the schema, then its name in the other language.
  Begin(s, 2);
  auto file = ReadAll(path);
  const auto named = NamedAfter(b, file, t, role, After(b, file, role), 3201, 2, "w3b_paths_object");
  ExpectInsert(b, s, path, Request(2, named.Rows()), "a named object below the schema");
  file = ReadAll(path);
  const auto foreign = EntryLike(named.entry, t, V7(After(b, file, role), 0xd10001), 1, 2, "w3b_paths_objet", other_language, true,
                                 c::CatalogNameClass::primary);
  ExpectInsert(b, s, path, Request(2, {foreign}), "its name in another language, through the schema's vector default");
  // An English alias of the schema, not its English default.
  file = ReadAll(path);
  const auto alias = EntryLike(entry_of(file, t.entry.language_uuid.value, true), t, V7(After(b, file, role), 0xd10002), 2, 2,
                               "w3b_paths_alias", t.entry.language_uuid.value, false, c::CatalogNameClass::alias);
  ExpectInsert(b, s, path, Request(2, {alias}), "an alias of the schema");
  Commit(*s.w, s.sink(), 2);
  ExpectNamedVerification(b, path, definitions + 1, "the further names, committed", false, true);
  const auto alias_entry = std::get<c::CatalogNameEntry>(*alias.name);
  // The expected moves, written out: entries below the schema in `language`
  // (or not in it), whose path's schema part `from` becomes `to`.
  const auto expect_moves = [&](std::span<const byte> before, std::span<const byte> after, const db::CatalogVersionWriteResult& result,
                                const c::CatalogNameEntry& from, const c::CatalogNameEntry& to, bool english_only,
                                const std::string& label) {
    std::vector<EntryVersion> want;
    for (const auto& [row, v] : NewestEntries(b, before))
      if (!v.deleted && v.entry.lifecycle_state == c::CatalogNameLifecycle::active &&
          StartsWith(v.entry.full_path_lookup_key, from.full_path_lookup_key) &&
          (v.entry.language_uuid.value == t.entry.language_uuid.value) == english_only)
        want.push_back(v);
    if (result.cascaded.size() != want.size())
      std::cerr << label << ": cascaded " << result.cascaded.size() << " expected " << want.size() << '\n';
    Check(result.cascaded.size() == want.size() && !want.empty(), label + ": the entries expected to move, and only those");
    const auto versions = EntryVersionsOf(b, after);
    const auto t2 = ReadTreeAt(b, after, NameRootPage(b, after, 23));
    for (std::size_t i = 0; i < want.size(); ++i) {
      Check(result.cascaded[i].first == want[i].row, label + ": in row-UUID order");
      const auto found = std::find_if(versions.begin(), versions.end(), [&](const auto& v) { return v.version == result.cascaded[i].second; });
      Check(found != versions.end(), label + ": the cascaded version is stored");
      auto key = to.full_path_lookup_key;
      key.insert(key.end(), want[i].entry.full_path_lookup_key.begin() + static_cast<std::ptrdiff_t>(from.full_path_lookup_key.size()),
                 want[i].entry.full_path_lookup_key.end());
      Check(found->entry.full_path_lookup_key == key, label + ": the path runs through the new name");
      bool cell = false;
      for (const auto leaf : t2.leaves)
        for (const auto& x : t2.pages[leaf].page.cells)
          if (x.key.version_uuid == found->version) {
            const auto payload = c::DecodeCatalogNameUuidTreeEntryPayload(x.payload);
            Check(payload && payload->display_components[from.path_component_count - 1] == to.display_name,
                  label + ": the UUID-to-name cell shows the new name");
            cell = true;
          }
      Check(cell, label + ": the cascaded version has its cell");
    }
  };
  const auto cascade_ids = [&](db::CatalogVersionRequest& r, unsigned k) {
    for (unsigned i = 0; i < 64; ++i) r.cascade_versions.push_back(V7(kT + 1500000 + 100 * u64{k} + i, 0x760000 + 64 * k + i));
    for (unsigned i = 0; i < 16; ++i)
      r.new_pages.push_back({V7(kT + 1600000 + 100 * u64{k} + i, 0xf70000 + k), V7(kT + 1600000 + 100 * u64{k} + i, 0xf80000 + k)});
  };
  // The schema's default name becomes the alias: four rows by one
  // transaction, since the default name is the definition's, the vector's,
  // and the one entry marked default for the object and for the vector's
  // language (the startup name rules). Each step's cascade is checked.
  Begin(s, 3);
  {
    const auto successor_of = [&](const db::CatalogInsertRow& row, unsigned k, c::CatalogNamePayload payload) {
      auto r = NameSuccessor(row, row.version_uuid, 3, VersionId(k), std::move(payload), false);
      cascade_ids(r, k - 9300);
      return r;
    };
    const auto write = [&](const db::CatalogVersionRequest& r, const std::string& label) {
      const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
      if (!result.ok()) std::cerr << label << ": " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.ok(), label + ": written");
      return result;
    };
    // 1. The English entry no longer the default: paths still reach it as
    //    the vector's default, so nothing moves.
    file = ReadAll(path);
    {
      const auto row = entry_of(file, t.entry.language_uuid.value, true);
      auto e = std::get<c::CatalogNameEntry>(*row.name);
      e.default_for_language = false;
      e.default_for_object = false;
      const auto result = write(successor_of(row, 9301, c::CatalogNamePayload{e}), "the English entry no longer the default");
      Check(result.cascaded.empty(), "a name still reached as the vector's default moves nothing");
    }
    // 2. The alias default for English and for the object: every English
    //    entry below moves onto it.
    file = ReadAll(path);
    {
      const auto row = NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
        const auto* e = std::get_if<c::CatalogNameEntry>(&p);
        return e && e->name_entry_uuid.value == alias_entry.name_entry_uuid.value;
      }, "the alias");
      auto e = std::get<c::CatalogNameEntry>(*row.name);
      e.default_for_language = true;
      e.default_for_object = true;
      const auto result = write(successor_of(row, 9302, c::CatalogNamePayload{e}), "the alias default for English");
      expect_moves(file, ReadAll(path), result, english, alias_entry, true, "the alias default for English");
    }
    // 3. The vector's default the alias: the entry in the other language,
    //    which runs through the vector's default, moves onto it.
    file = ReadAll(path);
    {
      const auto vector = NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
        const auto* v = std::get_if<c::CatalogNameVector>(&p);
        return v && v->object_uuid.value == schema;
      }, "the schema's vector");
      auto v = std::get<c::CatalogNameVector>(*vector.name);
      v.default_name_entry_uuid = alias_entry.name_entry_uuid;
      auto r = successor_of(vector, 9303, c::CatalogNamePayload{v});
      r.metadata.default_name_uuid = alias_entry.name_entry_uuid;
      const auto result = write(r, "the vector's default the alias");
      expect_moves(file, ReadAll(path), result, english, alias_entry, false, "the vector's default the alias");
    }
    // 4. The schema's definition names the alias as its default.
    file = ReadAll(path);
    {
      std::optional<db::CatalogVersionRequest> r;
      for (const u16 at : {u16{1}, u16{3}, u16{4}, u16{5}, u16{6}}) {
        RoleScope scope(b, at);
        const auto rel = ReadRelation(b, file);
        for (const auto& l : rel.leaves)
          for (const auto& row : l.leaf.body.rows)
            if (l.metadata.at(row.version_uuid).record.header.object_uuid.value == schema && !r) {
              const auto newest = rel.Versions(row.row_uuid.value).front();
              const auto& version = rel.At(newest.first).leaf.body.rows[newest.second].version_uuid;
              r = Successor(b, rel, row.row_uuid.value, version, 3, VersionId(9304));
            }
      }
      Check(r.has_value(), "the schema's definition is found");
      r->metadata.default_name_uuid = alias_entry.name_entry_uuid;
      cascade_ids(*r, 4);  // page identities, should its relation need a new leaf
      r->cascade_versions.clear();
      write(*r, "the schema's definition names the alias");
    }
    Commit(*s.w, s.sink(), 3);
    ExpectNamedVerification(b, path, definitions + 1, "the schema's default name changed and its cascades, committed", false, true);
  }
  // The old English entry, now on no path, dropped: nothing moves.
  Begin(s, 4);
  {
    file = ReadAll(path);
    const auto row = NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
      const auto* e = std::get_if<c::CatalogNameEntry>(&p);
      return e && e->name_entry_uuid.value == english.name_entry_uuid.value;
    }, "the old English entry");
    auto e = std::get<c::CatalogNameEntry>(*row.name);
    e.lifecycle_state = c::CatalogNameLifecycle::dropped;
    e.dropped_transaction_uuid = {UuidKind::transaction, TxId(4)};
    auto r = NameSuccessor(row, row.version_uuid, 4, VersionId(9320), c::CatalogNamePayload{e}, true);
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!result.ok()) std::cerr << "drop: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok() && result.cascaded.empty(), "dropping a name no path uses moves nothing and needs no identity");
    Commit(*s.w, s.sink(), 4);
    ExpectNamedVerification(b, path, definitions + 1, "the unused name dropped, committed", false, true);
  }
  std::cout << b.name << " name cascade paths page_size=" << b.size << ": default name changed (4 rows), unused drop\n";
  fs::remove(path);
}

// ---- Paths through another transaction's change (owner Q47 = 1) ----------------------------
// The template's schema's name in the template's language (its newest version,
// those of `dead` transactions left out), as a request row.
EntryVersion ScopeEntry(const Db& b, const NameTemplate& t, std::span<const byte> file, const std::set<u64>& dead = {}) {
  for (const auto& [row, v] : NewestEntries(b, file, dead))
    if (!v.deleted && v.entry.object_uuid.value == t.entry.scope_uuid.value && v.entry.language_uuid.value == t.entry.language_uuid.value &&
        v.entry.default_for_language)
      return v;
  Check(false, "the template's scope has a name in the template's language");
  return EntryVersion{};
}
// A rename of the template's schema by `n` (version VersionId(k)), with
// `identities` cascade identities of its own range and page identities.
db::CatalogVersionRequest ScopeRename(const Db& b, const NameTemplate& t, std::span<const byte> file, u64 n, unsigned k,
                                      std::size_t identities, const std::string& text, const std::set<u64>& dead = {}) {
  const auto old = ScopeEntry(b, t, file, dead);
  const auto row = NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
    const auto* e = std::get_if<c::CatalogNameEntry>(&p);
    return e && e->name_entry_uuid.value == old.entry.name_entry_uuid.value;
  }, "the scope's name row");
  auto e = Keyed(old.entry, t, text);
  PathBeside(e, old.entry);
  auto r = NameSuccessor(row, old.version, n, VersionId(k), c::CatalogNamePayload{e}, false);
  for (std::size_t i = 0; i < identities; ++i)
    r.cascade_versions.push_back(V7(kT + 1700000 + 1000 * u64{k} + i, 0x780000 + 4096 * (k % 64) + static_cast<unsigned>(i)));
  for (unsigned i = 0; i < 16; ++i)
    r.new_pages.push_back({V7(kT + 1800000 + 20 * u64{k} + i, 0xf90000 + k), V7(kT + 1800000 + 20 * u64{k} + i, 0xfa0000 + k)});
  return r;
}
std::size_t EntriesBelow(const Db& b, std::span<const byte> file, const std::vector<byte>& prefix) {
  std::size_t n = 0;
  for (const auto& [row, v] : NewestEntries(b, file))
    if (!v.deleted && v.entry.lifecycle_state == c::CatalogNameLifecycle::active && StartsWith(v.entry.full_path_lookup_key, prefix)) ++n;
  return n;
}
// SB_NATIVE_NAME_PATH_CONFLICT (owner Q47 = 1): a write whose path runs
// through a name another transaction is changing is a write_conflict with it,
// both ways round, and the right thing happens once that transaction ends:
//  A. the schema renamed by 2, in progress; 3 inserts a named object below it:
//     write_conflict with 2, no write. 2 rolls back: 3's insert succeeds. Or 2
//     commits: 3's insert, its path still through the old name, is
//     invalid_name; with the path through the new name it succeeds.
//  B. 2 inserts a named object below the schema, in progress; 3 renames the
//     schema: write_conflict with 2 (N4b). 2 rolls back: the rename succeeds,
//     cascading the entries below but 2's.
//  C. 2 inserts a named object X, in progress; 3 inserts a name scoped under
//     X: write_conflict with 2 (it was invalid_name: X's name was not in 3's
//     view). 2 rolls back: invalid_name (X has no name).
//  D. the schema renamed by 2, in progress; 3 renames a name no path below
//     the schema runs through: it succeeds (before Q47's fix the schema's
//     name left the path index, and everything below it went unnamed), both
//     commit, and the database verifies.
void NameConflicts(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  // A.
  for (const bool commit : {false, true}) {
    const auto path = b.Copy(b.bytes, "conflict-a.db");
    Session s(b, path);
    Begin(s, 2);
    auto file = ReadAll(path);
    const auto rename = ScopeRename(b, t, file, 2, 9400, 4096, "orders_renamed_a");
    const auto renamed = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), rename);
    Check(renamed.ok() && !renamed.cascaded.empty(), "A: the schema renamed by 2, cascading, in progress");
    Begin(s, 3);
    file = ReadAll(path);
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 3401, 3, "w3b_conflict_a");
    {
      const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), Request(3, named.Rows()));
      if (result.error != CE::write_conflict) std::cerr << "A: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.error == CE::write_conflict && result.blocker_order == u64{2} && !result.attempted && ReadAll(path) == file,
            "A: an insert below a schema another transaction is renaming is a write conflict with it, no write");
    }
    if (!commit) {
      Rollback(*s.w, s.sink(), 2);
      const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), Request(3, named.Rows()));
      if (!result.ok()) std::cerr << "A rollback: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.ok(), "A: the rename rolled back, the insert below the schema succeeds");
    } else {
      Commit(*s.w, s.sink(), 2);
      file = ReadAll(path);
      InsertRefused(b, s, path, Request(3, named.Rows()), CE::invalid_name,
                    "A: the rename committed, an insert whose path runs through the old name");
      auto rows = named.Rows();
      auto e = std::get<c::CatalogNameEntry>(*rows[2].name);
      const auto neighbour = NewestEntries(b, file).at(NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
        const auto* x = std::get_if<c::CatalogNameEntry>(&p);
        return x && x->name_entry_uuid.value == t.entry.name_entry_uuid.value;
      }, "the template's entry").row_uuid);
      PathBeside(e, neighbour.entry);
      rows[2].name = c::CatalogNamePayload{e};
      const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), Request(3, rows));
      if (!result.ok()) std::cerr << "A commit: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.ok(), "A: the rename committed, the insert with its path through the new name succeeds");
    }
    Commit(*s.w, s.sink(), 3);
    ExpectNamedVerification(b, path, definitions + 1, commit ? "A: rename committed, insert below" : "A: rename rolled back, insert below",
                            false, true);
    fs::remove(path);
  }
  // B.
  {
    const auto path = b.Copy(b.bytes, "conflict-b.db");
    Session s(b, path);
    Begin(s, 2);
    auto file = ReadAll(path);
    const auto below = EntriesBelow(b, file, ScopeEntry(b, t, file).entry.full_path_lookup_key);
    const auto named = NamedAfter(b, file, t, role, After(b, file, role), 3402, 2, "w3b_conflict_b");
    ExpectInsert(b, s, path, Request(2, named.Rows()), "B: a named object below the schema, in progress");
    Begin(s, 3);
    file = ReadAll(path);
    {
      const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), ScopeRename(b, t, file, 3, 9410, 4096, "orders_renamed_b"));
      Check(result.error == CE::write_conflict && result.blocker_order == u64{2} && !result.attempted && ReadAll(path) == file,
            "B: renaming the schema over another transaction's entry below it is a write conflict with it, no write");
    }
    Rollback(*s.w, s.sink(), 2);
    file = ReadAll(path);
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), ScopeRename(b, t, file, 3, 9411, 4096, "orders_renamed_b", {2}));
    if (!result.ok()) std::cerr << "B: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok() && result.cascaded.size() == below, "B: 2 rolled back, the rename cascades the entries below but 2's");
    Commit(*s.w, s.sink(), 3);
    // Only name rows committed (data, Q50): full verification passes.
    ExpectNamedVerification(b, path, definitions, "B: insert rolled back, rename committed", true, true);
    fs::remove(path);
  }
  // C.
  {
    const auto path = b.Copy(b.bytes, "conflict-c.db");
    Session s(b, path);
    Begin(s, 2);
    auto file = ReadAll(path);
    const auto parent = NamedAfter(b, file, t, role, After(b, file, role), 3403, 2, "w3b_conflict_parent");
    {
      // Through the writer: at 8 KiB the names relation may need a new leaf,
      // which the in-place oracle does not model.
      auto r = Request(2, parent.Rows());
      r.new_pages = PageIds(7);
      const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
      if (!result.ok()) std::cerr << "C: X: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.ok(), "C: a named object X, in progress");
    }
    Begin(s, 3);
    file = ReadAll(path);
    auto child = NamedAfter(b, file, t, role, After(b, file, role), 3404, 3, "w3b_conflict_child");
    const auto& x = std::get<c::CatalogNameEntry>(*parent.entry.name);
    auto e = std::get<c::CatalogNameEntry>(*child.entry.name);
    e.scope_uuid = x.object_uuid;
    e.full_path_lookup_key = x.full_path_lookup_key;
    const auto own = Escaped(OwnKey(e));
    e.full_path_lookup_key.insert(e.full_path_lookup_key.end(), own.begin(), own.end());
    e.path_component_count = x.path_component_count + 1;
    child.entry.name = c::CatalogNamePayload{e};
    child.definition.metadata.record.header.parent_uuid = x.object_uuid;
    {
      const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), Request(3, child.Rows()));
      if (result.error != CE::write_conflict) std::cerr << "C: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
      Check(result.error == CE::write_conflict && result.blocker_order == u64{2} && !result.attempted && ReadAll(path) == file,
            "C: a name under an object another transaction is creating is a write conflict with it, no write");
    }
    Rollback(*s.w, s.sink(), 2);
    InsertRefused(b, s, path, Request(3, child.Rows()), CE::invalid_name, "C: X rolled back, the name under it has no path");
    Rollback(*s.w, s.sink(), 3);
    fs::remove(path);
  }
  // D.
  {
    const auto path = b.Copy(b.bytes, "conflict-d.db");
    Session s(b, path);
    Begin(s, 2);
    auto file = ReadAll(path);
    const auto scope = ScopeEntry(b, t, file);
    const auto renamed = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), ScopeRename(b, t, file, 2, 9420, 4096, "orders_renamed_d"));
    Check(renamed.ok(), "D: the schema renamed by 2, in progress");
    Begin(s, 3);
    file = ReadAll(path);
    // A name of another object in the template's language and identifier
    // profile, not the database's, whose path does not run through the schema
    // and through which no path below the schema runs.
    std::optional<EntryVersion> other;
    for (const auto& [row, v] : NewestEntries(b, file))
      if (!other && !v.deleted && v.entry.lifecycle_state == c::CatalogNameLifecycle::active && v.entry.default_for_language &&
          v.entry.path_component_count >= 2 && v.entry.language_uuid.value == t.entry.language_uuid.value &&
          v.entry.identifier_profile_uuid.value == t.entry.identifier_profile_uuid.value &&
          !StartsWith(v.entry.full_path_lookup_key, scope.entry.full_path_lookup_key) &&
          !StartsWith(scope.entry.full_path_lookup_key, v.entry.full_path_lookup_key) && v.order != 2)
        other = v;
    Check(other.has_value(), "D: a name outside the schema's paths");
    const auto row = NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
      const auto* e = std::get_if<c::CatalogNameEntry>(&p);
      return e && e->name_entry_uuid.value == other->entry.name_entry_uuid.value;
    }, "the unrelated name's row");
    auto e = Keyed(other->entry, t, "w3b_unrelated_d");
    PathBeside(e, other->entry);
    auto r = NameSuccessor(row, other->version, 3, VersionId(9421), c::CatalogNamePayload{e}, false);
    for (unsigned i = 0; i < 4096; ++i) r.cascade_versions.push_back(V7(kT + 1900000 + i, 0x7c0000 + i));
    for (unsigned i = 0; i < 16; ++i) r.new_pages.push_back({V7(kT + 1950000 + i, 0xfb0000), V7(kT + 1950000 + i, 0xfc0000)});
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    if (!result.ok()) std::cerr << "D: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "D: a rename no path below the schema runs through succeeds while the schema's rename is in progress");
    Commit(*s.w, s.sink(), 3);
    Commit(*s.w, s.sink(), 2);
    // Only name rows committed (data, Q50): full verification passes.
    ExpectNamedVerification(b, path, definitions, "D: both renames committed", true, true);
    std::cout << b.name << " name conflicts page_size=" << b.size << ": D renamed " << other->entry.display_name << " ("
              << result.cascaded.size() << " cascaded) beside the schema's rename (" << renamed.cascaded.size() << ")\n";
    fs::remove(path);
  }
  std::cout << b.name << " name conflicts page_size=" << b.size << ": A (both endings), B, C, D\n";
}

// ---- Inserts that move paths (owner Q48 = 1) -----------------------------------------------
// SB_NATIVE_NAME_CASCADE for inserts: a name for the schema in a second
// language, marked default for it, moves the entries in that language below
// the schema (their paths ran through the schema's vector default), in the
// same write. Written out, not computed by the shared path rule: the entry
// below's new full path is the new name's full path then the rest of its old
// one after the vector default's. Also: the count needed when short; another
// transaction's entry below in that language while the insert is in progress
// (write_conflict, Q47); a second default in that language, which leaves the
// entry below ambiguous (invalid_name); failing each write in turn.
void NameInsertCascade(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "insert-cascade.db");
  Session s(b, path);
  const auto schema = t.entry.scope_uuid.value;
  const Uuid language = OtherLanguage(t.entry.language_uuid.value);
  const auto english_row = [&](std::span<const byte> file) {
    return NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
      const auto* e = std::get_if<c::CatalogNameEntry>(&p);
      return e && e->object_uuid.value == schema && e->language_uuid.value == t.entry.language_uuid.value && e->default_for_language;
    }, "the schema's default entry");
  };
  // A named object below the schema and its name in the second language.
  Begin(s, 2);
  auto file = ReadAll(path);
  const auto named = NamedAfter(b, file, t, role, After(b, file, role), 3501, 2, "w3b_insert_cascade");
  ExpectInsert(b, s, path, Request(2, named.Rows()), "a named object below the schema");
  file = ReadAll(path);
  const auto foreign = EntryLike(named.entry, t, V7(After(b, file, role), 0xd20001), 11, 2, "w3b_insert_objet", language, true,
                                 c::CatalogNameClass::primary);
  ExpectInsert(b, s, path, Request(2, {foreign}), "its name in the second language");
  Commit(*s.w, s.sink(), 2);
  const auto pre = ReadAll(path);
  const auto english = std::get<c::CatalogNameEntry>(*english_row(pre).name);
  const auto foreign_entry = std::get<c::CatalogNameEntry>(*foreign.name);
  Check(StartsWith(foreign_entry.full_path_lookup_key, english.full_path_lookup_key),
        "the second-language entry's path runs through the schema's vector default");
  // The schema's name in the second language, default for it, by `n`.
  const auto schema_name = [&](std::span<const byte> file, u64 n, unsigned k, const std::string& text, std::size_t identities) {
    auto row = EntryLike(english_row(file), t, V7(After(b, file, role), 0xd30000 + k), k, n, text, language, true,
                         c::CatalogNameClass::alias);
    auto r = Request(n, {row});
    for (std::size_t i = 0; i < identities; ++i) r.cascade_versions.push_back(V7(kT + 2000000 + 100 * u64{k} + i, 0x7d0000 + 64 * k + static_cast<unsigned>(i)));
    for (unsigned i = 0; i < 16; ++i)
      r.new_pages.push_back({V7(kT + 2100000 + 100 * u64{k} + i, 0xfd0000 + k), V7(kT + 2100000 + 100 * u64{k} + i, 0xfe0000 + k)});
    return std::pair{r, std::get<c::CatalogNameEntry>(*row.name)};
  };
  const auto check = [&](std::span<const byte> after, const db::CatalogVersionWriteResult& result, const c::CatalogNameEntry& name,
                         const std::string& label) {
    Check(result.cascaded.size() == 1 && result.cascaded[0].first == foreign.row_uuid, label + ": the second-language entry moves, only it");
    const auto versions = EntryVersionsOf(b, after);
    const auto found = std::find_if(versions.begin(), versions.end(), [&](const auto& v) { return v.version == result.cascaded[0].second; });
    Check(found != versions.end(), label + ": its version is stored");
    auto key = name.full_path_lookup_key;
    key.insert(key.end(), foreign_entry.full_path_lookup_key.begin() + static_cast<std::ptrdiff_t>(english.full_path_lookup_key.size()),
               foreign_entry.full_path_lookup_key.end());
    Check(found->entry.full_path_lookup_key == key && found->entry.path_component_count == foreign_entry.path_component_count,
          label + ": its path runs through the new name");
    const auto t2 = ReadTreeAt(b, after, NameRootPage(b, after, 23));
    bool cell = false;
    for (const auto leaf : t2.leaves)
      for (const auto& x : t2.pages[leaf].page.cells)
        if (x.key.version_uuid == found->version) {
          const auto payload = c::DecodeCatalogNameUuidTreeEntryPayload(x.payload);
          Check(payload && payload->display_components[english.path_component_count - 1] == name.display_name,
                label + ": the UUID-to-name cell shows the new name");
          cell = true;
        }
    Check(cell, label + ": the cascaded version has its cell");
  };
  Begin(s, 3);
  const auto begun = ReadAll(path);  // BEGIN wrote the inventory
  {
    auto [r, name] = schema_name(pre, 3, 21, "w3b_schema_nom", 0);
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
    std::cout << b.name << " insert cascade short: " << db::CatalogVersionWriteErrorName(result.error) << " needed "
              << result.cascade_needed << " failed_row " << (result.failed_row ? int(*result.failed_row) : -1) << '\n';
    Check(result.error == CE::cascade_versions_exhausted && result.cascade_needed == 1 && !result.failed_row && !result.attempted &&
              result.allocated_pages.empty() && ReadAll(path) == begun,
          "no identity: cascade_versions_exhausted, one needed, no row blamed, nothing written");
  }
  {
    auto [r, name] = schema_name(pre, 3, 21, "w3b_schema_nom", 4);
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
    if (!result.ok()) std::cerr << "insert cascade: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "the schema's second-language default name is inserted, cascading");
    check(ReadAll(path), result, name, "the cascading insert");
    std::cout << b.name << " insert cascade page_size=" << b.size << ": " << result.writes << " writes, "
              << result.allocated_pages.size() << " new pages\n";
  }
  // Q47 with it: another transaction's second-language name below the schema
  // while the insert is in progress.
  Begin(s, 4);
  {
    file = ReadAll(path);
    const auto alias = EntryLike(named.entry, t, V7(After(b, file, role), 0xd20002), 12, 4, "w3b_insert_alias", language, false,
                                 c::CatalogNameClass::alias);
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), Request(4, {alias}));
    Check(result.error == CE::write_conflict && result.blocker_order == u64{3} && !result.attempted && ReadAll(path) == file,
          "a second-language name below the schema while its second-language default is inserted: write conflict");
  }
  Rollback(*s.w, s.sink(), 4);
  Commit(*s.w, s.sink(), 3);
  ExpectNamedVerification(b, path, definitions + 1, "the cascading insert, committed", false, true);
  // A second default in that language: the entry below's scope is ambiguous.
  Begin(s, 5);
  {
    file = ReadAll(path);
    auto [r, name] = schema_name(file, 5, 22, "w3b_schema_nom_deux", 4);
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
    if (result.error != CE::invalid_name) std::cerr << "ambiguous: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.error == CE::invalid_name && !result.failed_row && !result.attempted && ReadAll(path) == file,
          "a second default in one language leaves the entry below ambiguous: invalid_name, nothing written");
  }
  Rollback(*s.w, s.sink(), 5);
  // The cascading insert again on copies of the file before it, failing each
  // write in turn; the database verifies; another transaction's insert then
  // cascades the same entry, and verifies.
  unsigned failures = 0;
  for (u32 w = 1; w <= 128; ++w) {
    const auto copy = b.Copy(pre, "insert-cascade-crash.db");
    Session c(b, copy);
    Begin(c, 3);
    auto [r, name] = schema_name(pre, 3, 23, "w3b_schema_nom", 4);
    KindFaultSink sink(c.sink(), KindFaultSink::Kind::write, w);
    const auto result = db::WriteCatalogInsertVersions(sink, GrowingBinding(b), r);
    if (result.ok()) {
      fs::remove(copy);
      break;
    }
    ++failures;
    Check(result.error == CE::write_failed, "a failed write is write_failed");
    Rollback(*c.w, c.sink(), 3);
    if (w <= 3 || w % 4 == 0) ExpectNamedVerification(b, copy, definitions + 1, "after failed insert-cascade write " + std::to_string(w), false, true);
    Begin(c, 4);
    const auto now = ReadAll(copy);
    auto [next, next_name] = schema_name(now, 4, 24, "w3b_schema_nom", 4);
    const auto done = db::WriteCatalogInsertVersions(c.sink(), GrowingBinding(b), next);
    if (!done.ok()) std::cerr << "after failed insert-cascade write " << w << ": " << db::CatalogVersionWriteErrorName(done.error) << '\n';
    Check(done.ok(), "the insert succeeds after failed write " + std::to_string(w));
    check(ReadAll(copy), done, next_name, "the insert after failed write " + std::to_string(w));
    Commit(*c.w, c.sink(), 4);
    if (w <= 3 || w % 4 == 0) ExpectNamedVerification(b, copy, definitions + 1, "the insert after failed write " + std::to_string(w), false, true);
    fs::remove(copy);
  }
  Check(failures > 0, "the cascading insert has failure points");
  std::cout << b.name << " insert cascade crashes page_size=" << b.size << " failure points " << failures << '\n';
  fs::remove(path);
}

// ---- The catalog-change audit event (W3b-2a, owner Q31 = 1, Q49 = 1, Q50 = 1) -------------
// The create's construction-intent row (role 6): the template an event row's
// metadata starts from (its owner, schema and parent), and its intent.
std::pair<c::CatalogMetadataVersion, c::CatalogConstructionIntent> ConstructionIntentOf(const Db& b, std::span<const byte> file) {
  RoleScope scope(b, 6);
  const auto rel = ReadRelation(b, file);
  for (const auto& l : rel.leaves)
    for (const auto& [version, m] : l.metadata)
      if (m.object_subtype == "node_construction_intent") {
        const auto intent = c::DecodeCatalogConstructionIntent(m.record.payload);
        Check(intent.ok(), "the create's construction intent decodes");
        return {m, *intent.record};
      }
  Check(false, "the create's construction intent is found");
  return {};
}
// A catalog.metadata_change event for transaction `n`, written as written by
// the caller: payload as given, row metadata from the construction intent's
// with the event as its object and its own audit anchor, first version by `n`.
c::CatalogMetadataChange ChangeEventPayload(const c::CatalogConstructionIntent& intent, const Uuid& event, u64 n, u64 generation) {
  c::CatalogMetadataChange p;
  p.event_uuid = event;
  p.database_uuid = intent.database_uuid;
  p.engine_uuid = intent.engine_uuid;
  p.origin_transaction_uuid = {UuidKind::transaction, TxId(n)};
  p.origin_local_transaction_id = n;
  p.event_time_utc_us = intent.event_time_utc_us + 1000 * n;
  p.catalog_generation = generation;
  p.event = c::MetadataChangeAuditEvent::metadata_change;
  p.event_class = c::MetadataChangeAuditClass::metadata_change;
  p.policy_decision = c::MetadataChangeAuditDecision::not_evaluated;
  p.redaction = c::MetadataChangeAuditRedaction::safe_message_only;
  return p;
}
db::CatalogInsertRow ChangeEventRow(const Db& b, std::span<const byte> file, u64 n, const Uuid& key, unsigned k,
                                    const std::function<void(c::CatalogMetadataChange&)>& edit = {}) {
  const auto [like, intent] = ConstructionIntentOf(b, file);
  const Uuid event = V7(Ms(key), 0x790000 + k);
  auto p = ChangeEventPayload(intent, event, n, like.catalog_generation);
  if (edit) edit(p);
  std::array<byte, c::kCatalogMetadataChangeMaxBytes> out{};
  const auto encoded = c::EncodeCatalogMetadataChangeInto(p, out);
  Check(encoded.ok(), "the event encodes");
  auto m = like;
  m.record.header.object_uuid = {UuidKind::object, event};
  m.record.header.row_uuid = {UuidKind::row, key};
  m.record.payload.assign(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size());
  m.object_subtype = "catalog_metadata_change";
  m.audit_uuid = {UuidKind::object, event};
  m.creator_transaction_uuid = {UuidKind::transaction, TxId(n)};
  m.creator_local_transaction_id = n;
  m.definition_version = 1;
  return {6, key, V7(Ms(key), 0x7a0000 + k), m, std::nullopt};
}
// The codec, against the registry's carrier (catalog_metadata_change_v1):
// extents 232 / 256, a round trip, and each refused value.
void MetadataChangeCodec(const Db& b) {
  const auto [like, intent] = ConstructionIntentOf(b, b.bytes);
  const auto event = V7(kT + 2200000, 0x790001);
  auto p = ChangeEventPayload(intent, event, 2, 1);
  std::array<byte, c::kCatalogMetadataChangeMaxBytes> out{};
  const auto encoded = c::EncodeCatalogMetadataChangeInto(p, out);
  Check(encoded.ok() && encoded.bytes.size() == 232 && c::CatalogMetadataChangeEncodedBytes(p) == std::optional<std::size_t>{232},
        "codec: 232 bytes without an actor, as the carrier states");
  const std::string_view bytes(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size());
  Check(c::IsCatalogMetadataChangePayload(bytes) && !c::IsCatalogConstructionIntentPayload(bytes), "codec: its own schema id");
  const auto back = c::DecodeCatalogMetadataChange(bytes);
  Check(back.ok() && back.record->event_uuid == event && back.record->origin_local_transaction_id == 2 &&
            back.record->database_uuid == intent.database_uuid && !back.record->actor_uuid,
        "codec: a round trip");
  auto with_actor = p;
  with_actor.actor_uuid = scratchbird::core::platform::TypedUuid{UuidKind::principal, V7(kT + 2200001, 0x790002)};
  Check(c::CatalogMetadataChangeEncodedBytes(with_actor) == std::optional<std::size_t>{256}, "codec: 256 bytes with an actor");
  const std::vector<std::pair<std::string, std::function<void(c::CatalogMetadataChange&)>>> refused{
      {"no origin number", [](auto& r) { r.origin_local_transaction_id = 0; }},
      {"no event time", [](auto& r) { r.event_time_utc_us = 0; }},
      {"no catalog generation", [](auto& r) { r.catalog_generation = 0; }},
      {"another decision", [](auto& r) { r.policy_decision = static_cast<c::MetadataChangeAuditDecision>(2); }},
      {"another class", [](auto& r) { r.event_class = static_cast<c::MetadataChangeAuditClass>(2); }},
      {"another name", [](auto& r) { r.event = static_cast<c::MetadataChangeAuditEvent>(2); }},
      {"another redaction", [](auto& r) { r.redaction = static_cast<c::MetadataChangeAuditRedaction>(2); }},
      {"the engine is the database", [&](auto& r) { r.engine_uuid = r.database_uuid; }},
      {"the origin is not a transaction", [](auto& r) { r.origin_transaction_uuid.kind = UuidKind::object; }},
      {"an actor that is not a principal",
       [](auto& r) { r.actor_uuid = scratchbird::core::platform::TypedUuid{UuidKind::object, V7(kT + 2200002, 0x790003)}; }}};
  for (const auto& [what, edit] : refused) {
    auto r = p;
    edit(r);
    std::array<byte, c::kCatalogMetadataChangeMaxBytes> o{};
    Check(!c::EncodeCatalogMetadataChangeInto(r, o).ok() && !c::CatalogMetadataChangeEncodedBytes(r), "codec refuses: " + what);
  }
  std::cout << b.name << " metadata change codec page_size=" << b.size << ": 232/256 bytes, " << refused.size() << " refusals\n";
}
// SB_AUDIT_CATALOG_METADATA_CHANGE (W3b-2a, W3b-2b): a committed change whose
// definition cites its transaction's event passes FULL open verification; name
// rows being data (Q50), a rename alone also passes. The writer refuses, before
// any write: an event that is another transaction's or another database's, a
// construction intent after the create; a definition citing no event of its own
// transaction (none written yet, the create's construction intent, another
// transaction's committed event: unaudited_change), an insert's or a
// successor's; a successor of an event row (events are immutable).
void AuditedChanges(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  MetadataChangeCodec(b);
  const auto path = b.Copy(b.bytes, "audited.db");
  Session s(b, path);
  Begin(*s.w, s.sink(), 2);  // no event at BEGIN: this transaction writes its own below
  auto file = ReadAll(path);
  const auto key = V7(After(b, file, 6), 0xd40001);
  const auto [intent_row, intent] = ConstructionIntentOf(b, file);
  // Refusals first, nothing written.
  {
    auto other = ChangeEventRow(b, file, 2, key, 1, [](auto& p) {
      p.origin_transaction_uuid = {UuidKind::transaction, TxId(3)};
      p.origin_local_transaction_id = 3;
    });
    InsertRefused(b, s, path, Request(2, {other}), CE::invalid_insert, "an event naming another transaction as its origin");
    auto foreign = ChangeEventRow(b, file, 2, key, 1, [](auto& p) { p.database_uuid = V7(kT + 2200003, 0x790004); });
    InsertRefused(b, s, path, Request(2, {foreign}), CE::invalid_insert, "an event of another database");
    auto late = intent_row;
    late.record.header.object_uuid = {UuidKind::object, V7(Ms(key), 0x790010)};
    late.record.header.row_uuid = {UuidKind::row, key};
    late.creator_transaction_uuid = {UuidKind::transaction, TxId(2)};
    late.creator_local_transaction_id = 2;
    InsertRefused(b, s, path, Request(2, {db::CatalogInsertRow{6, key, V7(Ms(key), 0x7a0010), late, std::nullopt}}),
                  CE::invalid_insert, "a construction intent written after the create");
    const auto bare = NamedAfter(b, file, t, role, After(b, file, role), 3600, 2, "w3b_no_event_object");
    InsertRefused(b, s, path, Request(2, bare.Rows()), CE::unaudited_change,
                  "a definition by a transaction that has written no event");
  }
  // The event and a named object whose definition cites it, in one request.
  const auto event_row = ChangeEventRow(b, file, 2, key, 1);
  auto named = NamedAfter(b, file, t, role, After(b, file, role), 3601, 2, "w3b_audited_object");
  named.definition.metadata.audit_uuid = event_row.metadata.record.header.object_uuid;
  // Its name rows' references resolve to it too (open verification requires a
  // name row's audit reference to resolve; the writer does not check it).
  named.vector.metadata.audit_uuid = named.entry.metadata.audit_uuid = event_row.metadata.record.header.object_uuid;
  auto request = Request(2, {event_row, named.definition, named.vector, named.entry});
  request.new_pages = PageIds(31);
  {
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), request);
    if (!result.ok()) std::cerr << "audited insert: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "the event and a named object citing it are inserted");
  }
  Commit(*s.w, s.sink(), 2);
  ExpectNamedVerification(b, path, definitions + 1, "an audited named object, committed", true, true);
  // A rename (name rows are data): committed, passes.
  Begin(s, 3);
  file = ReadAll(path);
  {
    const auto result = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), ScopeRename(b, t, file, 3, 9430, 4096, "orders_audited"));
    if (!result.ok()) std::cerr << "rename: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok() && !result.cascaded.empty(), "the schema renamed, cascading");
  }
  Commit(*s.w, s.sink(), 3);
  ExpectNamedVerification(b, path, definitions + 1, "a rename (name rows are data), committed", true, true);
  // 4 (its event written at BEGIN): definitions citing another event refused.
  Begin(s, 4);
  file = ReadAll(path);
  auto next = NamedAfter(b, file, t, role, After(b, file, role), 3602, 4, "w3b_second_audited_object");
  {
    // Its path through the schema's new name.
    auto e = std::get<c::CatalogNameEntry>(*next.entry.name);
    const auto neighbour = NewestEntries(b, file).at(NameRowOf(b, file, [&](const c::CatalogNamePayload& p) {
      const auto* x = std::get_if<c::CatalogNameEntry>(&p);
      return x && x->name_entry_uuid.value == t.entry.name_entry_uuid.value;
    }, "the template's entry").row_uuid);
    PathBeside(e, neighbour.entry);
    next.entry.name = c::CatalogNamePayload{e};
  }
  {
    auto cites_create = next;
    cites_create.definition.metadata.audit_uuid = intent_row.audit_uuid;
    InsertRefused(b, s, path, Request(4, cites_create.Rows()), CE::unaudited_change, "a definition citing the create's construction intent");
    auto cites_other = next;
    cites_other.definition.metadata.audit_uuid = event_row.metadata.record.header.object_uuid;
    InsertRefused(b, s, path, Request(4, cites_other.Rows()), CE::unaudited_change,
                  "a definition citing another transaction's committed event");
  }
  {
    // Successors: tx 2's definition, citing tx 2's event; tx 2's event row.
    RoleScope scope(b, role);
    const auto rel = ReadRelation(b, file);
    auto r = Successor(b, rel, named.definition.row_uuid, named.definition.version_uuid, 4, VersionId(9440));
    r.metadata.audit_uuid = event_row.metadata.record.header.object_uuid;
    const auto before = ReadAll(path);
    const auto refused = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    Check(refused.error == CE::unaudited_change && !refused.attempted && ReadAll(path) == before,
          "a successor citing another transaction's event: unaudited_change, nothing written");
  }
  {
    RoleScope scope(b, 6);
    const auto rel = ReadRelation(b, file);
    auto r = Successor(b, rel, event_row.row_uuid, event_row.version_uuid, 4, VersionId(9441));
    r.metadata.audit_uuid = event_row.metadata.record.header.object_uuid;
    const auto before = ReadAll(path);
    const auto refused = db::WriteCatalogSuccessorVersion(s.sink(), GrowingBinding(b), r);
    Check(refused.error == CE::invalid_successor && !refused.attempted && ReadAll(path) == before,
          "a successor of an event row (immutable): invalid_successor, nothing written");
  }
  {
    auto r = Request(4, next.Rows());
    r.new_pages = PageIds(32);
    const auto result = db::WriteCatalogInsertVersions(s.sink(), GrowingBinding(b), r);
    if (!result.ok()) std::cerr << "own event: " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.ok(), "a definition citing its transaction's event (written at BEGIN) is inserted");
  }
  Commit(*s.w, s.sink(), 4);
  ExpectNamedVerification(b, path, definitions + 2, "a second audited named object, committed", true, true);
  std::cout << b.name << " audited changes page_size=" << b.size << ": audited inserts and a rename pass full; unaudited definitions refused at write\n";
  fs::remove(path);
}

void NameSuccessors(const Db& b, const NameTemplate& t, u16 role, std::size_t definitions) {
  const auto path = b.Copy(b.bytes, "successors.db");
  Session s(b, path);
  Begin(s, 2);
  const auto file = ReadAll(path);
  const auto named = NamedAfter(b, file, t, role, After(b, file, role), 1, 2, "w3b_renamed");
  if (!InsertOracle(b, file, Request(2, named.Rows()))) {
    std::cout << b.name << " successors page_size=" << b.size << " no room (W3b-3)\n";
    fs::remove(path);
    return;
  }
  ExpectInsert(b, s, path, Request(2, named.Rows()), "a named object to rename");
  Commit(*s.w, s.sink(), 2);
  const auto entry_of = [&](const std::string& text) {
    auto e = Keyed(std::get<c::CatalogNameEntry>(*named.entry.name), t, text);
    PathBeside(e, t.entry);
    return c::CatalogNamePayload{e};
  };
  // A rename: the old version's cells stamped, the new version's added.
  Begin(s, 3);
  const auto renamed = NameSuccessor(named.entry, named.entry.version_uuid, 3, VersionId(9001), entry_of("w3b_renamed_v2"), false);
  if (!ExpectNameSuccessor(b, s, path, renamed, "a renamed entry by a transaction in progress")) {
    Rollback(*s.w, s.sink(), 3);
    fs::remove(path);
    return;
  }
  {
    RoleScope a(b, 2);
    PeekSees(b, path, named.entry.row_uuid, named.entry.version_uuid, "the old name while the rename is in progress");
  }
  Commit(*s.w, s.sink(), 3);
  {
    RoleScope a(b, 2);
    PeekSees(b, path, named.entry.row_uuid, VersionId(9001), "the new name, committed");
  }
  ExpectNamedVerification(b, path, definitions + 1, "a renamed entry, committed");
  // A rename to a name another object holds in the scope: duplicate_name.
  Begin(s, 4);
  Refused(b, s, path, NameSuccessor(AsRow(renamed), VersionId(9001), 4, VersionId(9002), entry_of(t.entry.raw_name_text), false),
          CE::duplicate_name, "a rename onto the template's name");
  // A rename rolled back: its stamp stays on the visible version's cells; the
  // engine admits a stamp from a transaction that did not commit.
  const bool rolled = ExpectNameSuccessor(b, s, path, NameSuccessor(AsRow(renamed), VersionId(9001), 4, VersionId(9003), entry_of("w3b_renamed_v3"), false),
                                          "a rename then rolled back");
  Rollback(*s.w, s.sink(), 4);
  (void)rolled;
  {
    RoleScope a(b, 2);
    PeekSees(b, path, named.entry.row_uuid, VersionId(9001), "the committed name after a rolled-back rename");
  }
  ExpectNamedVerification(b, path, definitions + 1, "a rolled-back rename");
  Begin(s, 5);
  // (N4a refused renaming a name other names' paths run through; N4b
  // cascades it: NameCascade.)
  // A drop: the definition, the vector and the entry retired together; the
  // entry's and vector's cells stamped, no new cells.
  {
    auto e = std::get<c::CatalogNameEntry>(*renamed.name);
    e.lifecycle_state = c::CatalogNameLifecycle::dropped;
    e.dropped_transaction_uuid = scratchbird::core::platform::TypedUuid{UuidKind::transaction, TxId(5)};
    auto entry_drop = NameSuccessor(AsRow(renamed), VersionId(9001), 5, VersionId(9004), c::CatalogNamePayload{e}, true);
    entry_drop.metadata.record.payload.clear();
    if (!ExpectNameSuccessor(b, s, path, entry_drop, "the entry dropped")) {
      Rollback(*s.w, s.sink(), 5);
      fs::remove(path);
      return;
    }
    auto v = std::get<c::CatalogNameVector>(*named.vector.name);
    v.lifecycle_state = c::CatalogNameLifecycle::dropped;
    if (!ExpectNameSuccessor(b, s, path, NameSuccessor(named.vector, named.vector.version_uuid, 5, VersionId(9005), c::CatalogNamePayload{v}, true),
                             "the vector dropped")) {
      Rollback(*s.w, s.sink(), 5);
      fs::remove(path);
      return;
    }
    RoleScope a(b, role);
    const auto rel = ReadRelation(b, ReadAll(path));
    Expect(b, s, path, Successor(b, rel, named.definition.row_uuid, named.definition.version_uuid, 5, VersionId(9006), true),
           "the definition dropped");
  }
  Commit(*s.w, s.sink(), 5);
  ExpectNamedVerification(b, path, definitions, "a dropped named object", true);
  fs::remove(path);
  std::cout << b.name << " name successors page_size=" << b.size << '\n';
}

// ---- Collisions ---------------------------------------------------------------------------
std::string Upper(std::string text) {
  for (auto& ch : text)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return text;
}
void NameCollisions(const Db& b, const NameTemplate& t, u16 role, bool room) {
  const auto path = b.Copy(b.bytes, "collisions.db");
  Session s(b, path);
  Begin(s, 2);
  Begin(s, 3);
  const auto file = ReadAll(path);
  const auto ms = After(b, file, role);
  // The template's own name, in its scope: a committed holder.
  InsertRefused(b, s, path, Request(2, NamedAfter(b, file, t, role, ms, 1, 2, t.entry.raw_name_text).EntryFirst()),
                CE::duplicate_name, "the name a committed object holds");
  const auto upper = Upper(t.entry.raw_name_text);
  const bool folds = Keyed(t.entry, t, upper).normalized_lookup_key == t.entry.normalized_lookup_key;
  if (folds && upper != t.entry.raw_name_text)
    InsertRefused(b, s, path, Request(2, NamedAfter(b, file, t, role, ms, 2, 2, upper).EntryFirst()), CE::duplicate_name,
                  "the same name in another case (the profile folds it)");
  if (!room) {
    std::cout << b.name << " name collisions page_size=" << b.size << " committed holder only (no room)\n";
    fs::remove(path);
    return;
  }
  // Two entries of one request, one key.
  {
    auto first = NamedAfter(b, file, t, role, ms, 3, 2, "w3b_twice");
    auto second = NamedAfter(b, file, t, role, ms, 4, 2, "w3b_twice");
    auto rows = first.Rows();
    for (const auto& r : second.Rows()) rows.push_back(r);
    const auto before = ReadAll(path);
    const auto result = Insert(b, s.sink(), Request(2, rows));
    Check(result.error == CE::duplicate_name && result.failed_row == 5u && result.writes == 0 && ReadAll(path) == before,
          "two entries of one request with one key: the second refused duplicate_name");
  }
  // A name another transaction in progress holds.
  const auto x = NamedAfter(b, file, t, role, ms, 5, 2, "w3b_contended");
  ExpectInsert(b, s, path, Request(2, x.Rows()), "a name inserted by transaction 2, in progress");
  const auto conflict = [&](u64 n, const Named& named, u64 blocker, std::string_view label) {
    const auto before = ReadAll(path);
    const auto result = Insert(b, s.sink(), Request(n, named.Rows()));
    if (result.error != CE::write_conflict) std::cerr << label << ": got " << db::CatalogVersionWriteErrorName(result.error) << '\n';
    Check(result.error == CE::write_conflict && result.blocker_order == blocker && result.failed_row == 2u &&
              result.writes == 0 && ReadAll(path) == before,
          std::string(label) + ": write_conflict naming the holder, with no write");
  };
  conflict(3, NamedAfter(b, ReadAll(path), t, role, ms, 6, 3, "w3b_contended"), 2,
           "the name of transaction 2's insert in progress");
  InsertRefused(b, s, path, Request(2, NamedAfter(b, ReadAll(path), t, role, ms, 7, 2, "w3b_contended").Rows()),
                CE::duplicate_name, "a name this transaction already inserted");
  Rollback(*s.w, s.sink(), 2);
  const auto y = NamedAfter(b, ReadAll(path), t, role, ms, 8, 3, "w3b_contended");
  ExpectInsert(b, s, path, Request(3, y.Rows()), "the name, once its holder rolled back");
  Begin(s, 4);
  const auto z = NamedAfter(b, ReadAll(path), t, role, ms, 9, 4, "w3b_contended");
  conflict(4, z, 3, "the name of transaction 3's insert in progress");
  Commit(*s.w, s.sink(), 3);
  InsertRefused(b, s, path, Request(4, z.Rows()), CE::duplicate_name, "a name a committed transaction inserted");
  Rollback(*s.w, s.sink(), 4);
  std::cout << b.name << " name collisions page_size=" << b.size << " folds=" << folds << '\n';
  fs::remove(path);
}

// ---- Refusals before any write -------------------------------------------------------------
void NameRefusals(const Db& b, const NameTemplate& t, u16 role) {
  const auto path = b.Copy(b.bytes, "name-refusals.db");
  Session s(b, path);
  const u64 n = 2;
  Begin(s, n);
  const auto file = ReadAll(path);
  const auto ms = After(b, file, role);
  const auto good = NamedAfter(b, file, t, role, ms, 1, n, "w3b_refused");
  const auto entry = [&](auto change, CE error, std::string_view label) {
    auto named = good;
    auto e = std::get<c::CatalogNameEntry>(*named.entry.name);
    change(e, named);
    named.entry.name = c::CatalogNamePayload{e};
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), error, label);
  };
  entry([](auto& e, auto&) { e.normalized_lookup_key.push_back(byte{'x'}); }, CE::invalid_name,
        "a normalized key the profile does not derive");
  entry([](auto& e, auto&) { e.display_name += "x"; }, CE::invalid_name, "a display name the profile does not derive");
  entry([](auto& e, auto&) { e.dialect_profile_uuid.value = V7(1, 2); }, CE::invalid_name, "an unknown dialect profile");
  entry([](auto& e, auto&) { e.identifier_profile_uuid.value = V7(1, 3); }, CE::invalid_name,
        "an identifier profile the dialect profile does not name");
  entry([](auto& e, auto&) { e.case_fold_profile_uuid = scratchbird::core::platform::TypedUuid{UuidKind::object, V7(1, 4)}; }, CE::invalid_name,
        "a case-fold supplement");
  // SB_NATIVE_NAME_PATH: the stored full path is the resolved one.
  entry([](auto& e, auto&) { e.full_path_lookup_key.back() = byte{1}; }, CE::invalid_name,
        "a full path whose last component is not the entry's own key");
  entry([](auto& e, auto&) { e.full_path_lookup_key.insert(e.full_path_lookup_key.begin(), {byte{'x'}, byte{0}, byte{0}}); },
        CE::invalid_name, "a full path with a component above the scope");
  entry([](auto& e, auto&) { ++e.path_component_count; }, CE::invalid_name, "a full path counting one component too many");
  entry([](auto& e, auto&) { e.full_path_lookup_key.clear(); e.path_component_count = 0; }, CE::invalid_name,
        "an entry without its full path");
  entry([](auto& e, auto& named) {
    e.name_vector_uuid.value = V7(1, 5);
    named.entry.metadata.name_vector_uuid.value = V7(1, 5);
    named.entry.metadata.record.header.parent_uuid.value = V7(1, 5);
  }, CE::invalid_name, "an entry whose vector does not exist");
  {
    auto named = good;
    auto v = std::get<c::CatalogNameVector>(*named.vector.name);
    v.name_collision_policy_uuid.value = V7(1, 6);
    named.vector.name = c::CatalogNamePayload{v};
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_name, "a vector naming no collision policy");
  }
  // Relations.
  {
    auto named = good;
    named.vector.role = role;
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_role, "a name row outside the names relation");
    auto row = good.definition;
    row.role = 2;
    InsertRefused(b, s, path, Request(n, {row}), CE::invalid_role, "a policy row in the names relation");
    row = good.definition;
    row.role = 4;
    row.metadata.record.header.kind = c::CatalogRecordKind::audit_evidence;
    InsertRefused(b, s, path, Request(n, {row}), CE::invalid_role, "audit evidence outside role 6");
    row = good.definition;
    row.role = 4;
    row.metadata.record.header.kind = c::CatalogRecordKind::schema;
    InsertRefused(b, s, path, Request(n, {row}), CE::invalid_role, "a schema outside role 1");
  }
  // Name presence, payload and binding.
  {
    auto named = good;
    named.entry.name.reset();
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_insert, "a name row without its name");
    named = good;
    named.definition.name = good.entry.name;
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_insert, "a definition carrying a name");
    named = good;
    named.entry.metadata.record.payload = "x";
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_insert, "a name row with its own payload");
    named = good;
    auto v = std::get<c::CatalogNameVector>(*named.vector.name);
    v.object_uuid.value = V7(1, 7);
    named.vector.name = c::CatalogNamePayload{v};
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_insert, "a vector that disagrees with its metadata");
    named = good;
    auto e = std::get<c::CatalogNameEntry>(*named.entry.name);
    e.created_transaction_uuid.value = TxId(n + 1);
    named.entry.name = c::CatalogNamePayload{e};
    InsertRefused(b, s, path, Request(n, named.EntryFirst()), CE::invalid_insert, "an entry created by another transaction");
    auto row = good.definition;
    row.metadata.record.header.kind = c::CatalogRecordKind::database;
    row.role = 1;
    InsertRefused(b, s, path, Request(n, {row}), CE::invalid_insert, "a database record");
  }
  fs::remove(path);
}

// ---- One class in one scope under two collision policies ----------------------------------
// Startup refuses two entries of one class, scope and identifier profile
// under different collision policy bodies (conflicting_policy); the writer
// refuses invalid_name. A second descriptor and body, copies of the
// database's under new identities, are inserted by the transaction, which
// sees its own rows; its named object of the template's class and scope
// whose vector names the second descriptor is refused, and the same object
// under the database's policy is admitted.
void ConflictingPolicy(const Db& b, const NameTemplate& t, u16 role) {
  const auto path = b.Copy(b.bytes, "conflicting-policy.db");
  Session s(b, path);
  const u64 n = 2;
  Begin(s, n);
  const auto file = ReadAll(path);
  const auto ms = std::max(After(b, file, role), After(b, file, 5));
  std::optional<c::CatalogMetadataVersion> dm, bm;
  {
    RoleScope scope(b, 5);
    const auto rel = ReadRelation(b, file);
    for (const auto& l : rel.leaves)
      for (const auto& [version, m] : l.metadata)
        if (m.record.header.object_uuid.value == t.vector.name_collision_policy_uuid.value) dm = m;
    Check(dm.has_value(), "the database's collision policy descriptor");
    const auto descriptor = c::DecodeCatalogPolicyDescriptor(dm->record.payload);
    Check(descriptor.ok(), "the descriptor decodes");
    for (const auto& l : rel.leaves)
      for (const auto& [version, m] : l.metadata)
        if (m.record.header.object_uuid.value == descriptor.record->body_uuid) bm = m;
    Check(bm.has_value(), "the database's collision policy body");
  }
  const scratchbird::core::platform::TypedUuid tx{UuidKind::transaction, TxId(n)};
  auto body = *c::DecodeCatalogNamingCollisionPolicy(bm->record.payload).record;
  body.body_uuid = V7(ms, 0xf00001);
  body.generation = 1;
  for (std::size_t g = 0; g < body.group_count; ++g) {
    const auto fresh = V7(ms, 0xf10000 + static_cast<unsigned>(g));
    for (std::size_t k = 0; k < body.class_count; ++k)
      if (body.classes[k].group_uuid == body.groups[g].group_uuid) body.classes[k].group_uuid = fresh;
    body.groups[g].group_uuid = fresh;
  }
  body.origin_transaction_uuid = body.creating_transaction_uuid = tx;
  body.origin_local_transaction_id = body.creating_local_transaction_id = n;
  auto descriptor = *c::DecodeCatalogPolicyDescriptor(dm->record.payload).record;
  descriptor.policy_uuid = V7(ms, 0xf00002);
  descriptor.policy_version_uuid = V7(ms, 0xf00003);
  descriptor.body_uuid = body.body_uuid;
  descriptor.body_generation = 1;
  descriptor.generation = 1;
  descriptor.creating_transaction_number = n;
  descriptor.creating_transaction_uuid = descriptor.origin_transaction_uuid = tx;
  descriptor.origin_local_transaction_id = n;
  const auto row = [&](c::CatalogMetadataVersion m, const Uuid& object, unsigned k, std::vector<byte> payload) {
    m.record.header.object_uuid = {UuidKind::object, object};
    m.record.header.row_uuid = {UuidKind::row, V7(ms, 0xf20000 + k)};
    m.record.payload.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    m.definition_version = 1;
    m.creator_transaction_uuid = tx;
    m.creator_local_transaction_id = n;
    m.audit_uuid = EventRef(n);  // W3b-2b: the transaction's event
    m.default_name_uuid = {UuidKind::object, V7(ms, 0xf30000 + k)};
    m.name_vector_uuid = {UuidKind::object, V7(ms, 0xf40000 + k)};
    db::CatalogInsertRow r;
    r.role = 5;
    r.row_uuid = m.record.header.row_uuid.value;
    r.version_uuid = VersionId(9000 + k);
    r.metadata = std::move(m);
    return r;
  };
  std::vector<byte> body_bytes(*c::CatalogNamingCollisionPolicyEncodedBytes(body));
  Check(c::EncodeCatalogNamingCollisionPolicyInto(body, body_bytes).ok(), "the second body encodes");
  std::vector<byte> descriptor_bytes(*c::CatalogPolicyDescriptorEncodedBytes(descriptor));
  Check(c::EncodeCatalogPolicyDescriptorInto(descriptor, descriptor_bytes).ok(), "the second descriptor encodes");
  const auto body_row = row(*bm, body.body_uuid, 1, body_bytes);
  const auto descriptor_row = row(*dm, descriptor.policy_uuid, 2, descriptor_bytes);
  Check(c::CatalogNamingCollisionPolicyMatchesMetadata(c::BorrowCatalogMetadataVersion(body_row.metadata)) &&
            c::CatalogPolicyDescriptorMatchesMetadata(c::BorrowCatalogMetadataVersion(descriptor_row.metadata)),
        "the second body and descriptor agree with their metadata");
  const auto policy = Request(n, {body_row, descriptor_row});
  if (!InsertOracle(b, file, policy)) {
    std::cout << b.name << " conflicting policy page_size=" << b.size << " skipped: no room in role 5 for a second policy\n";
    fs::remove(path);
    return;
  }
  ExpectInsert(b, s, path, policy, "a second collision policy, inserted by the transaction");
  const auto named = NamedAfter(b, ReadAll(path), t, role, ms + 1, 1, n, "w3b_other_policy");
  auto other = named;
  auto v = std::get<c::CatalogNameVector>(*other.vector.name);
  v.name_collision_policy_uuid = {UuidKind::object, descriptor.policy_uuid};
  other.vector.name = c::CatalogNamePayload{v};
  InsertRefused(b, s, path, Request(n, other.EntryFirst()), CE::invalid_name,
                "one class in one scope under a second collision policy");
  // The name rows grew with the language constants (SB_CATALOG_LANGUAGE_CONSTANTS):
  // where the names leaf has no room left, the same insert takes a new leaf.
  auto same = Request(n, named.Rows());
  if (InsertOracle(b, ReadAll(path), same)) {
    ExpectInsert(b, s, path, same, "the same object under the database's policy");
  } else {
    same.new_pages = PageIds(41);
    ExpectGrowingInsert(b, s.sink(), path, same, "the same object under the database's policy, on a new page");
  }
  std::cout << b.name << " conflicting policy page_size=" << b.size << " refused\n";
  fs::remove(path);
}

// ---- Name rows inside and below the names relation's keys ---------------------------------
void NameKeys(const Db& b, const NameTemplate& t, u16 role) {
  const auto path = b.Copy(b.bytes, "name-keys.db");
  RoleScope scope(b, 2);
  const auto file = ReadAll(path);
  const auto tree = ReadTree(b, file);
  const auto ms = After(b, file, role);
  u64 n = 2;
  bool between = false, below = false, below_full = false;
  // The vector and entry between two keys of a leaf with room for two cells.
  for (const auto i : tree.leaves) {
    const auto& leaf = tree.pages[i];
    if (between || FreeCells(b, file, leaf.number) < 2 || leaf.page.cells.size() < 3) continue;
    for (std::size_t k = 0; k + 1 < leaf.page.cells.size() && !between; ++k) {
      const auto first = Between(leaf.page.cells[k].key.row_uuid, leaf.page.cells[k + 1].key.row_uuid);
      if (!first) continue;
      const auto second = Between(*first, leaf.page.cells[k + 1].key.row_uuid);
      if (!second) continue;
      Session s(b, path);
      Begin(s, n);
      const auto named = NamedObject(b, ReadAll(path), t, role, V7(ms + 1, 0xe00001), *first, *second, 11, n,
                                     "w3b_between_keys");
      const auto request = Request(n, named.Rows());
      if (!InsertOracle(b, ReadAll(path), request)) {
        Rollback(*s.w, s.sink(), n++);
        continue;
      }
      ExpectInsert(b, s, path, request, "names between two keys");
      Commit(*s.w, s.sink(), n++);
      PeekSees(b, path, named.entry.row_uuid, named.entry.version_uuid, "names between two keys, committed");
      between = true;
    }
  }
  // Below the smallest key.
  {
    const auto now = ReadAll(path);
    const auto fresh = ReadTree(b, now);
    const auto& leftmost = fresh.pages[fresh.leaves.front()];
    const auto lowest = Ms(leftmost.page.cells.front().key.row_uuid);
    Session s(b, path);
    Begin(s, n);
    const auto named = NamedObject(b, now, t, role, V7(ms + 2, 0xe00002), V7(lowest - 1, 0xe00003),
                                   V7(lowest - 1, 0xe00004), 12, n, "w3b_below_keys");
    const auto request = Request(n, named.Rows());
    if (FreeCells(b, now, leftmost.number) < 2) {
      InsertRefused(b, s, path, request, CE::index_full, "names below the smallest key, its leaf full");
      below_full = true;
    } else if (InsertOracle(b, now, request)) {
      ExpectInsert(b, s, path, request, "names below the smallest key");
      Commit(*s.w, s.sink(), n);
      PeekSees(b, path, named.entry.row_uuid, named.entry.version_uuid, "names below the smallest key, committed");
      below = true;
    }
  }
  std::cout << b.name << " name keys page_size=" << b.size << " between=" << between << " below=" << below
            << " index_full_below=" << below_full << '\n';
  fs::remove(path);
}
}  // namespace

int main(int argc, char** argv) {
  unsigned profile = 0;
  try {
    if (argc == 3 && std::string(argv[1]) == "--profile") profile = static_cast<unsigned>(std::stoul(argv[2]));
    Check(profile < d::kCanonicalFilespacePageProfiles.size(), "a canonical page profile");
    const auto production = ProductionDb(profile);
    const auto& b = *production;
    const auto fresh = Verify(b, b.Copy(b.bytes, "fresh.db"));
    Check(fresh.full.ok(), "the production database passes full catalog verification");
    const auto t = Template(b, b.bytes);
    const auto places = Places(b, b.bytes);
    const auto role = DefinitionRole(places);
    Check(role.has_value(), "a relation with room for the definition");
    {
      // The names relation (role 2) against the startup complete-scan tier.
      RoleScope scope(b, 2);
      const auto rel = ReadRelation(b, b.bytes);
      std::size_t leaves = rel.leaves.size(), rows = 0;
      for (const auto& l : rel.leaves) rows += l.leaf.body.rows.size();
      std::cout << b.name << " names relation page_size=" << b.size << ": " << leaves << " base pages of "
                << db::NativeCatalogScanBasePages(b.size) << ", " << rows << " rows of " << db::kNativeCatalogScanRelationMaximumRows
                << " (startup complete-scan tier)\n";
    }
    std::cout << b.name << " names page_size=" << b.size << " definition role=" << *role
              << " template=" << t.entry.raw_name_text << '\n';
    const bool room = NamedStates(b, t, *role, fresh.names.definitions);
    NameCollisions(b, t, *role, room);
    NameRefusals(b, t, *role);
    ConflictingPolicy(b, t, *role);
    if (room) NameKeys(b, t, *role);
    if (room) NameSuccessors(b, t, *role, fresh.names.definitions);
    if (room) NameCascade(b, t, *role, fresh.names.definitions);
    if (room) NameCascadePaths(b, t, *role, fresh.names.definitions);
    if (room) NameConflicts(b, t, *role, fresh.names.definitions);
    if (room) NameInsertCascade(b, t, *role, fresh.names.definitions);
    if (room) AuditedChanges(b, t, *role, fresh.names.definitions);
    if (room) NameGrowth(b, t, *role, fresh.names.definitions);
    if (room) NameSplits(b, t, *role, fresh.names.definitions);
    // Long names past a root-branch split and a branch split below the root:
    // at 8 KiB only, on a database with room for them (2,048 pages); at the
    // larger sizes 600 long-named objects split no branch (measured).
    if (room && b.size == 8192) {
      const auto big = ProductionDb(profile, 2048);
      const auto big_fresh = Verify(*big, big->Copy(big->bytes, "fresh-big.db"));
      const auto big_role = DefinitionRole(Places(*big, big->bytes));
      Check(big_role.has_value(), "a relation with room for the definition, 2,048 pages");
      NameBranchSplits(*big, Template(*big, big->bytes), *big_role, big_fresh.names.definitions);
    }
    std::cout << "PASS native catalog name insert page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog name insert profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
