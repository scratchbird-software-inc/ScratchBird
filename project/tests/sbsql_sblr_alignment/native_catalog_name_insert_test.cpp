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

#include "catalog_identifier_lookup_key.hpp"
#include "catalog_identifier_profile.hpp"
#include "catalog_naming_collision_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include "catalog_records.hpp"
#include <memory>
#include "native_catalog_cohort_capture.hpp"
#include "native_catalog_cohort_lease_reader.hpp"
#include "engine/internal_api/catalog/native_catalog_metadata_verification.hpp"

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
  const std::string policy = c::CatalogRecordKindName(c::CatalogRecordKind::policy);
  for (const auto& e : names.entries) {
    if (e.object_class != policy || !e.default_for_object || e.lifecycle_state != c::CatalogNameLifecycle::active) continue;
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
  e.language_tag = v.default_language_tag;
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
  ia::NativeCatalogNameVerification names;
  ia::NativeCatalogSecurityReferenceVerification security;
  ia::NativeCatalogNamingProfileVerification profiles;
  ia::NativeCatalogCollisionVerification collisions;
};
// On a copy of the file as it stands (a writing session keeps its device open).
Verified Verify(const Db& b, const std::string& original) {
  const auto path = b.Copy(ReadAll(original), "verify.db");
  d::FileDevice device;
  Check(device.Open(path, d::FileOpenMode::open_existing_read_only).ok(), "independent read-only open");
  Verified out;
  {
    const std::vector<d::NativeFilespaceDevice> files{{b.primary, b.profile, &device}};
    Grant g(b.database, 512u << 20);
    auto fixed = db::AcquireNativeFixedRootMemoryLease(b.database, files, b.primary, {512 * u64{b.size}, 64u << 20},
                                                       g.memory, g.binding);
    Check(fixed.ok(), "the fixed-root lease admits the file");
    // The shared complete-scan tier and the capture bounds startup uses.
    // SB_NATIVE_NAME_TREES: the production database has the name trees.
    const db::NativeCatalogRelationMemoryLimits limits{b.profile, 1, db::NativeCatalogScanBasePages(b.size),
                                                       db::kNativeCatalogScanRelationMaximumRows,
                                                       db::kNativeCatalogScanRelationMaximumRows,
                                                       db::NativeCatalogScanNavigationLimits(b.size),
                                                       db::NativeNameTreeNavigationLimits(b.size, db::kNativeCatalogScanRelationMaximumRows)};
    auto cohort = db::PrepareNativeCatalogCohortRead(limits, g.memory, g.binding);
    Check(cohort.ok(), "cohort workspace");
    // SB_NATIVE_NAME_TREES: the cells too, for the engine's name tree check.
    auto capture = db::PrepareNativeCatalogCapture({4096, 8u << 20, 1, 1024, 1024, 128, 8192}, g.memory, g.binding);
    Check(capture.ok(), "capture workspace");
    const auto read = db::NativeCatalogCohortLeaseReader::Read(*fixed.lease, 256u << 20, cohort.workspace, &capture.capture);
    if (!read.ok()) std::cerr << "cohort read error=" << int(read.error) << '\n';
    Check(read.ok(), "the committed catalog captures");
    const auto count = capture.capture.rows().size();
    std::vector<std::size_t> order(count), parents(count);
    std::vector<byte> colors(count);
    const ia::NativeCatalogNamespaceWorkspace workspace{order, parents, colors};
    out.full = ia::VerifyNativeCatalogMetadata(capture.capture, workspace);
    out.names = ia::VerifyNativeCatalogNameBindings(capture.capture, order);
    out.security = ia::VerifyNativeCatalogSecurityReferences(capture.capture, order);
    out.profiles = ia::VerifyNativeCatalogNamingProfileReferences(capture.capture, order);
    out.collisions = ia::VerifyNativeCatalogNamingCollisions(capture.capture, workspace);
  }
  Check(device.Close().ok(), "close the verifier's open");
  fs::remove(path);
  return out;
}
// A committed named object: every verifier but audit accepts it, and full
// verification refuses at exactly audit event_binding (W3b-2's flip target).
void ExpectNamedVerification(const Db& b, const std::string& path, std::size_t definitions, std::string_view label) {
  const auto v = Verify(b, path);
  using AE = ia::NativeCatalogAuditError;
  if (!v.names.ok()) std::cerr << label << " names=" << int(v.names.error) << '\n';
  if (!v.profiles.ok()) std::cerr << label << " profiles=" << int(v.profiles.error) << '\n';
  if (!v.collisions.ok()) std::cerr << label << " collisions=" << int(v.collisions.error) << '\n';
  if (!v.security.ok()) std::cerr << label << " security=" << int(v.security.error) << '\n';
  Check(v.names.ok() && v.names.definitions == definitions, std::string(label) + ": name verification accepts it");
  Check(v.profiles.ok() && v.collisions.ok() && v.security.ok(),
        std::string(label) + ": naming-profile, collision and security verification accept it");
  Check(v.full.names.ok() && v.full.indexes.ok() && v.full.namespaces.ok() && v.full.filespaces.ok() &&
            v.full.audit.error == AE::event_binding && !v.full.ok(),
        std::string(label) + ": full verification refuses at exactly audit event_binding (W3b-2)");
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
  Begin(*s.w, s.sink(), 2);
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
  Begin(*s.w, s.sink(), 3);
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

// ---- Collisions ---------------------------------------------------------------------------
std::string Upper(std::string text) {
  for (auto& ch : text)
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  return text;
}
void NameCollisions(const Db& b, const NameTemplate& t, u16 role, bool room) {
  const auto path = b.Copy(b.bytes, "collisions.db");
  Session s(b, path);
  Begin(*s.w, s.sink(), 2);
  Begin(*s.w, s.sink(), 3);
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
  Begin(*s.w, s.sink(), 4);
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
  Begin(*s.w, s.sink(), n);
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
  Begin(*s.w, s.sink(), n);
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
  ExpectInsert(b, s, path, Request(n, named.Rows()), "the same object under the database's policy");
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
      Begin(*s.w, s.sink(), n);
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
    Begin(*s.w, s.sink(), n);
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
    std::cout << b.name << " names page_size=" << b.size << " definition role=" << *role
              << " template=" << t.entry.raw_name_text << '\n';
    const bool room = NamedStates(b, t, *role, fresh.names.definitions);
    NameCollisions(b, t, *role, room);
    NameRefusals(b, t, *role);
    ConflictingPolicy(b, t, *role);
    if (room) NameKeys(b, t, *role);
    std::cout << "PASS native catalog name insert page_size=" << b.size << " checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL native catalog name insert profile=" << profile << ": " << e.what() << " checks=" << checks
              << '\n';
    return 1;
  }
}
