// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_catalog_version_writer.hpp"

#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_identifier_profile.hpp"
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
#include <map>
#include <new>
#include <set>

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

bool V7(const Uuid& id) { return scratchbird::core::uuid::IsEngineIdentityUuid(id); }

// The relations the startup verifiers place records in: the names relation
// (role 2) holds exactly the localized-name records (name verification);
// audit evidence is in role 6 (audit); database, schema and filespace records
// in role 1 (namespaces, filespaces); inline private-key policies in role 3
// (the create).
bool RoleAdmits(u16 role, const catalog::CatalogMetadataVersion& m) {
  using K = catalog::CatalogRecordKind;
  const auto kind = m.record.header.kind;
  if ((role == 2) != (kind == K::localized_name)) return false;
  if (kind == K::audit_evidence && role != 6) return false;
  if ((kind == K::database || kind == K::schema || kind == K::filespace) && role != 1) return false;
  if ((m.object_subtype == "crypto_inline_private_key" ||
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
// The record kind a name's object_class names (CatalogRecordKindName).
std::optional<catalog::CatalogRecordKind> KindNamed(std::string_view name) {
  for (unsigned k = 1; k < 256; ++k) {
    const auto kind = static_cast<catalog::CatalogRecordKind>(k);
    const char* text = catalog::CatalogRecordKindName(kind);
    if (text && name == text && kind != catalog::CatalogRecordKind::unknown) return kind;
  }
  return std::nullopt;
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
bool Follows(const catalog::CatalogMetadataVersion& before, const catalog::CatalogMetadataVersion& after,
             const CatalogVersionRequest& request) {
  const auto& b = before.record.header;
  const auto& a = after.record.header;
  if (catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(after))) return false;
  return a.kind == b.kind && a.object_uuid.kind == b.object_uuid.kind && a.object_uuid.value == b.object_uuid.value &&
         a.row_uuid.kind == UuidKind::row && a.row_uuid.value == request.row_uuid &&
         after.authority_scope != catalog::CatalogAuthorityScope::cluster &&
         after.creator_transaction_uuid.kind == UuidKind::transaction &&
         after.creator_transaction_uuid.value == request.transaction_uuid &&
         after.creator_local_transaction_id == request.transaction_order &&
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
};
// A relation as its head reaches it: the index pages (empty when unindexed),
// in walk order, and every base leaf, sorted by page number.
struct Relation {
  std::vector<TreePage> tree;
  std::vector<disk::NativePageReference> bases;
  std::vector<Leaf> leaves;  // parallel to bases
};
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
    // The index, walked as the tree reader walks it, read through the sink.
    struct Pending {
      disk::NativePageReference ref;
      std::optional<std::size_t> parent;
      u16 level = 0;
      std::optional<pg::NativeBtreeKey> low, high;
    };
    auto& tree = out.tree;
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
      auto decoded = pg::DecodeNativeBtreePage(image);
      if (!decoded.ok()) return refuse(E::invalid_catalog);
      auto& page = *decoded.page;
      if (!dependencies) {
        if (page.dependencies.index_uuid != head.object_uuid) return refuse(E::invalid_catalog);
        dependencies = page.dependencies;
      } else if (page.dependencies != *dependencies) {
        return refuse(E::invalid_catalog);
      }
      const auto previous = last_by_level.find(page.tree_level);
      if (pg::detail::ValidateNativeBtreeTraversalStep(
              page, next.parent ? &tree[*next.parent].page : nullptr, next.level, next.low, next.high,
              previous == last_by_level.end() ? nullptr : &tree[previous->second].page) != pg::NativeBtreeError::none)
        return refuse(E::invalid_catalog);
      const auto index = tree.size();
      last_by_level[page.tree_level] = index;
      tree.push_back({next.ref, image, std::move(page)});
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
    auto decoded = DecodeNativeCatalogLeaf(leaves[i].image);
    if (!decoded.ok()) {
      result.error = E::invalid_catalog;
      result.leaf_error = decoded.error;
      return false;
    }
    const auto& id = decoded.page->body.relation_uuid.value;
    if (head.page_type == 6 ? id != head.object_uuid : relation && *relation != id) return refuse(E::invalid_catalog);
    relation = id;
    leaves[i].page = std::move(*decoded.page);
    leaves[i].metadata = std::move(decoded.metadata);
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
    case E::name_record_unsupported: return "name_record_unsupported";
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
  }
  return "unknown";
}

CatalogVersionWriteResult WriteCatalogSuccessorVersion(disk::CarefulWriteSink& sink,
                                                       const CatalogVersionWriterBinding& binding,
                                                       const CatalogVersionRequest& request) noexcept {
  CatalogVersionWriteResult result;
  try {
    // ---- the request alone -------------------------------------------------
    const auto& croot = binding.catalog_root;
    const auto* profile = disk::FindCanonicalFilespacePageProfile(croot.page_size_profile_uuid);
    if (!V7(binding.database_uuid) || (croot.kind != 2 && croot.kind != 8) || !croot.page_number ||
        !croot.page_generation || !profile || binding.state_root.kind != 4 ||
        binding.state_root.page_size_profile_uuid != croot.page_size_profile_uuid ||
        binding.state_root.filespace_uuid != croot.filespace_uuid || !binding.maximum_chain_pages ||
        !binding.maximum_index_pages || !binding.maximum_base_pages || request.role < 1 || request.role > 6 ||
        !request.transaction_order || !V7(request.transaction_uuid) || !V7(request.row_uuid) ||
        !V7(request.predecessor_version_uuid) || !V7(request.version_uuid) ||
        request.version_uuid == request.predecessor_version_uuid || request.version_uuid == request.row_uuid)
      return result;
    if (binding.read_only_database) return Refuse(E::read_only_database);
    if (request.metadata.record.header.kind == catalog::CatalogRecordKind::localized_name)
      return Refuse(E::name_record_unsupported);
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
    Relation relation_read;
    if (!ReadRelation(sink, binding, page_size, *head, relation_read, result)) return result;
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
    if (!Follows(base_metadata, request.metadata, request)) {
      result.error = E::invalid_successor;
      return result;
    }
    {
      // A runtime authority binding's payload catalog_generation and
      // security_epoch equal its metadata's (that family's own rule, checked
      // above), so this also keeps every binding within the root.
      if (AheadOfRoot(request.metadata, *root.root)) return Refuse(E::ahead_of_catalog_root);
    }
    for (const auto& leaf : leaves)
      for (const auto& row : leaf.page.body.rows)
        if (row.version_uuid == request.version_uuid) return Refuse(E::invalid_successor);

    // ---- the new row -------------------------------------------------------
    const auto encoded = catalog::EncodeCatalogMetadataVersion(request.metadata);
    if (!encoded.ok()) return Refuse(E::invalid_successor);
    pg::RowDataRecord added;
    added.row_uuid = {UuidKind::row, request.row_uuid};
    added.version_uuid = request.version_uuid;
    added.previous_version_uuid = base_row.version_uuid;
    added.transaction_uuid = {UuidKind::transaction, request.transaction_uuid};
    added.local_transaction_id = request.transaction_order;
    added.row_version = newest + 1;
    added.previous_row_version = base_row.row_version;
    scratchbird::core::datatypes::DatatypeBinaryValue cell;
    cell.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
    cell.payload = encoded.bytes;
    added.cells = {{1, std::move(cell), {}}};

    // The predecessor's page with the forward link (a hint to readers).
    const auto link = [&](NativeCatalogLeafPage page) {
      auto& row = page.body.rows[base->row];
      row.next_version_uuid = request.version_uuid;
      row.next_row_version = added.row_version;
      return page;
    };
    // Placement: the predecessor's page first, then the others in page order.
    std::vector<std::size_t> order{base->leaf};
    for (std::size_t i = 0; i < leaves.size(); ++i)
      if (i != base->leaf) order.push_back(i);
    std::optional<std::size_t> target;
    NativeCatalogLeafResult target_image, link_image;
    for (const auto i : order) {
      auto page = i == base->leaf ? link(leaves[i].page) : leaves[i].page;
      u32 slot = 0;
      for (const auto& row : page.body.rows) slot = std::max(slot, row.stable_slot_id);
      if (slot == std::numeric_limits<u32>::max()) continue;
      auto row = added;
      row.stable_slot_id = slot + 1;
      row.storage_generation = page.header.page_generation;
      page.body.rows.push_back(std::move(row));
      const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
      if (!fits.ok()) {
        if (fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE") continue;
        if (fits.resource_failure()) return Refuse(E::resource_exhausted);
        return Refuse(E::invalid_successor);
      }
      target_image = EncodeNativeCatalogLeaf(page);
      if (!target_image.ok()) {
        result.error = target_image.error == NativeCatalogLeafError::resource_exhausted ? E::resource_exhausted
                                                                                        : E::invalid_successor;
        result.leaf_error = target_image.error;
        return result;
      }
      target = i;
      break;
    }
    if (!target) return Refuse(E::relation_full);
    if (*target != base->leaf) {
      link_image = EncodeNativeCatalogLeaf(link(leaves[base->leaf].page));
      if (!link_image.ok()) {
        result.error = link_image.error == NativeCatalogLeafError::resource_exhausted ? E::resource_exhausted
                                                                                      : E::invalid_catalog;
        result.leaf_error = link_image.error;
        return result;
      }
    }

    // ---- the writes --------------------------------------------------------
    disk::CarefulWritePlan plan;
    const auto offset = [&](std::size_t i) { return bases[i].page_number * u64{page_size}; };
    const auto first = plan.Add(offset(*target), std::span<const byte>(target_image.bytes),
                                std::span<const byte>(leaves[*target].image));
    if (!first.ok()) return Refuse(E::plan_refused);
    if (*target != base->leaf) {
      const auto second = plan.Add(offset(base->leaf), std::span<const byte>(link_image.bytes),
                                   std::span<const byte>(leaves[base->leaf].image));
      if (!second.ok() || plan.Before(first.node, second.node) != CarefulWriteError::none)
        return Refuse(E::plan_refused);
    }
    const auto r = plan.Execute(sink, binding.barrier_policy);
    result.write_error = r.error;
    result.io = r.io;
    result.writes = r.written;
    result.syncs = r.syncs;
    result.page_number = bases[*target].page_number;
    result.forward_link_page = bases[base->leaf].page_number;
    result.row_version = added.row_version;
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

CatalogVersionWriteResult WriteCatalogInsertVersions(disk::CarefulWriteSink& sink,
                                                     const CatalogVersionWriterBinding& binding,
                                                     const CatalogInsertRequest& request) noexcept {
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
      if (row.role < 1 || row.role > 6 || !V7(row.row_uuid) || !V7(row.version_uuid) || row.version_uuid == row.row_uuid)
        return refuse_row(E::invalid_request, i);
    }
    if (binding.read_only_database) return Refuse(E::read_only_database);
    // A name row carries its name and an empty payload (the envelope is
    // encoded once placed); the relation admits the record's kind.
    bool any_entry = false;
    for (std::size_t i = 0; i < request.rows.size(); ++i) {
      const auto& row = request.rows[i];
      const auto& m = row.metadata;
      const bool name = m.record.header.kind == catalog::CatalogRecordKind::localized_name;
      if (name != row.name.has_value() || (name && !m.record.payload.empty()) ||
          m.record.header.kind == catalog::CatalogRecordKind::database)
        return refuse_row(E::invalid_insert, i);
      if (!RoleAdmits(row.role, m)) return refuse_row(E::invalid_role, i);
      if (name && !catalog::CatalogNamePayloadMatchesMetadata(*row.name, m)) return refuse_row(E::invalid_insert, i);
      any_entry = any_entry || (name && std::holds_alternative<catalog::CatalogNameEntry>(*row.name));
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
    for (const auto role : roles) {
      if (relations.contains(role)) continue;
      const auto head = std::find_if(root.root->roots.begin(), root.root->roots.end(),
                                     [&](const auto& r) { return r.role == role; });
      if (head == root.root->roots.end() || (head->page_type != 6 && head->page_type != 0x200))
        return Refuse(E::invalid_catalog);
      heads[role] = *head;
      if (!ReadRelation(sink, binding, page_size, *head, relations[role], result)) return result;
    }

    // ---- each row: checked, placed, indexed --------------------------------
    // The pages a placement changes, by (role, index): their pages as changed.
    std::set<std::pair<u16, std::size_t>> changed_leaves, changed_tree;
    // A row's hold on its object: none once its creator rolled back or died;
    // pending while another transaction that created it is in progress;
    // otherwise held (committed, or this transaction's own).
    enum class Hold { none, pending, held };
    const auto hold = [&](const pg::RowDataRecord& row) {
      const auto* creator = entry(row.local_transaction_id);
      if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value) return Hold::held;
      const auto state = mga::InventoryVisibilityState(*creator);
      if (state == mga::TransactionState::rolled_back || state == mga::TransactionState::failed_terminal)
        return Hold::none;
      return InProgress(state) && row.local_transaction_id != request.transaction_order ? Hold::pending : Hold::held;
    };

    // ---- names: what this transaction sees of profiles, policies, names ---
    const auto state_of = [&](const pg::RowDataRecord& row) -> std::optional<mga::TransactionState> {
      const auto* creator = entry(row.local_transaction_id);
      if (!creator || creator->identity.transaction_uuid.value != row.transaction_uuid.value) return std::nullopt;
      return mga::InventoryVisibilityState(*creator);
    };
    const auto dead = [](const std::optional<mga::TransactionState>& state) {
      return state && (*state == mga::TransactionState::rolled_back || *state == mga::TransactionState::failed_terminal);
    };
    // The newest version of an object this transaction sees (its creator
    // committed, or its own), unless deleted.
    const auto current = [&](u16 role, const Uuid& object) -> const catalog::CatalogMetadataVersion* {
      const auto rel = relations.find(role);
      if (rel == relations.end()) return nullptr;
      const catalog::CatalogMetadataVersion* best = nullptr;
      u64 newest = 0;
      for (const auto& leaf : rel->second.leaves)
        for (const auto& row : leaf.page.body.rows) {
          const auto m = leaf.metadata.find(row.version_uuid);
          if (m == leaf.metadata.end() || m->second.record.header.object_uuid.value != object) continue;
          const auto state = state_of(row);
          if (!state || (*state != mga::TransactionState::committed && row.local_transaction_id != request.transaction_order))
            continue;
          if (!best || row.row_version > newest) {
            best = &m->second;
            newest = row.row_version;
          }
        }
      return best && !best->record.header.deleted ? best : nullptr;
    };
    const auto active = [](const catalog::CatalogMetadataVersion& m) {
      return m.status == catalog::CatalogObjectStatus::active && m.lifecycle == catalog::CatalogObjectLifecycle::active;
    };
    const auto view = [](const catalog::CatalogMetadataVersion& m) { return catalog::BorrowCatalogMetadataVersion(m); };
    // Every stored name version, its envelope checked against where it is.
    struct StoredName {
      catalog::CatalogNamePayload payload;
      Uuid row, object;
      u64 row_version = 0, order = 0;
      std::optional<mga::TransactionState> state;
      bool deleted = false;
    };
    std::vector<StoredName> stored;
    std::map<Uuid, std::size_t> newest_name;  // per row: the newest version not rolled back or dead
    if (any_entry && relations.contains(2)) {
      for (const auto& leaf : relations.at(2).leaves)
        for (const auto& row : leaf.page.body.rows) {
          const auto m = leaf.metadata.find(row.version_uuid);
          if (m == leaf.metadata.end()) return Refuse(E::invalid_catalog);
          const catalog::CatalogNameVersionBinding at{
              {UuidKind::database, leaf.page.header.database_uuid}, {UuidKind::filespace, leaf.page.header.filespace_uuid},
              row.row_uuid, {UuidKind::row, row.version_uuid}, m->second.record.header.object_uuid, row.transaction_uuid,
              leaf.page.body.page_number, row.stable_slot_id, row.storage_generation, row.row_version,
              row.local_transaction_id, m->second.catalog_generation};
          const auto& bytes = m->second.record.payload;
          const auto decoded = catalog::DecodeCatalogNameEnvelopeView(
              {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, at);
          if (!decoded.ok()) return Refuse(E::invalid_catalog);
          stored.push_back({catalog::MaterializeCatalogNameEnvelope(*decoded.record).payload, row.row_uuid.value,
                            m->second.record.header.object_uuid.value, row.row_version, row.local_transaction_id,
                            state_of(row), m->second.record.header.deleted});
        }
      for (std::size_t k = 0; k < stored.size(); ++k) {
        if (dead(stored[k].state)) continue;
        const auto found = newest_name.find(stored[k].row);
        if (found == newest_name.end() || stored[found->second].row_version < stored[k].row_version)
          newest_name[stored[k].row] = k;
      }
    }
    // The vector an entry names: in the request, else the newest stored
    // version (whoever wrote it: another transaction's entry in progress is
    // judged by its own vector).
    const auto vector_of = [&](const Uuid& id) -> const catalog::CatalogNameVector* {
      for (const auto& row : request.rows)
        if (row.name)
          if (const auto* v = std::get_if<catalog::CatalogNameVector>(&*row.name); v && v->name_vector_uuid.value == id)
            return v;
      for (const auto& [row, k] : newest_name)
        if (stored[k].object == id && !stored[k].deleted)
          if (const auto* v = std::get_if<catalog::CatalogNameVector>(&stored[k].payload)) return v;
      return nullptr;
    };
    // The collision group an entry's class binds to, as startup resolves it:
    // vector -> policy descriptor -> naming collision policy body -> class.
    struct Group {
      Uuid body, group;
      catalog::CatalogCollisionLanguageLane language{};
      catalog::CatalogCollisionKeyLane key{};
    };
    const auto group_of = [&](const catalog::CatalogNameEntry& e) -> std::optional<Group> {
      const auto* v = vector_of(e.name_vector_uuid.value);
      if (!v || v->object_uuid.value != e.object_uuid.value || v->object_class != e.object_class) return std::nullopt;
      const auto* dm = current(5, v->name_collision_policy_uuid.value);
      if (!dm || !active(*dm) || !catalog::CatalogPolicyDescriptorMatchesMetadata(view(*dm)) ||
          !catalog::CatalogPolicyDescriptorMatchesDatabase(view(*dm), binding.database_uuid))
        return std::nullopt;
      const auto policy = catalog::DecodeCatalogPolicyDescriptor(dm->record.payload);
      if (!policy.record || policy.record->policy_class != catalog::CatalogPolicyClass::metadata ||
          policy.record->scope != catalog::CatalogPolicyScope::database ||
          policy.record->scope_uuid != binding.database_uuid)
        return std::nullopt;
      const auto* bm = current(5, policy.record->body_uuid);
      if (!bm || !active(*bm) ||
          !(bm->object_subtype == "naming_collision_policy_body" ||
            catalog::IsCatalogNamingCollisionPolicyPayload(bm->record.payload)) ||
          !catalog::CatalogNamingCollisionPolicyMatchesMetadata(view(*bm)) ||
          !catalog::CatalogNamingCollisionPolicyMatchesDatabase(view(*bm), binding.database_uuid))
        return std::nullopt;
      const auto body = catalog::DecodeCatalogNamingCollisionPolicy(bm->record.payload);
      const auto kind = KindNamed(e.object_class);
      if (!body.ok() || body.record->generation != policy.record->body_generation || !kind) return std::nullopt;
      const auto classes = std::span(body.record->classes).first(body.record->class_count);
      const auto bound = std::find_if(classes.begin(), classes.end(), [&](const auto& c) { return c.kind == *kind; });
      if (bound == classes.end()) return std::nullopt;
      const auto groups = std::span(body.record->groups).first(body.record->group_count);
      const auto group = std::find_if(groups.begin(), groups.end(),
                                      [&](const auto& g) { return g.group_uuid == bound->group_uuid; });
      if (group == groups.end()) return std::nullopt;
      return Group{policy.record->body_uuid, group->group_uuid, group->language_lane, group->key_lane};
    };
    // An entry's keys, as startup checks them: its dialect profile, that
    // profile's identifier profile, and the keys under it.
    const auto keys_agree = [&](const catalog::CatalogNamePayload& payload) {
      const auto& e = std::get<catalog::CatalogNameEntry>(payload);
      if (e.case_fold_profile_uuid || e.quoted_identifier_profile_uuid) return false;
      const auto* dm = current(5, e.dialect_profile_uuid.value);
      if (!dm || !active(*dm) || dm->record.header.kind != catalog::CatalogRecordKind::config_profile ||
          dm->object_subtype != "naming_dialect_profile" || !catalog::CatalogNamingDialectProfileMatchesMetadata(view(*dm)) ||
          !catalog::CatalogNamingDialectProfileMatchesDatabase(view(*dm), binding.database_uuid))
        return false;
      const auto dialect = catalog::DecodeCatalogNamingDialectProfile(dm->record.payload);
      if (!dialect.record || dialect.record->identifier_profile_uuid != e.identifier_profile_uuid.value) return false;
      const auto* im = current(5, dialect.record->identifier_profile_uuid);
      if (!im || !active(*im) || im->record.header.kind != catalog::CatalogRecordKind::config_profile ||
          im->object_subtype != "identifier_resolution_profile" ||
          !catalog::CatalogIdentifierProfileMatchesMetadata(view(*im)) ||
          !catalog::CatalogIdentifierProfileMatchesDatabase(view(*im), binding.database_uuid))
        return false;
      const auto identifier = catalog::DecodeCatalogIdentifierProfile(im->record.payload);
      if (!identifier.record || identifier.record->generation != dialect.record->identifier_profile_generation ||
          (identifier.record->dialect_mode_flags & ~dialect.record->allowed_dialect_mode_flags))
        return false;
      return catalog::CheckCatalogIdentifierLookupKeys(
                 *identifier.record, std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(payload))) ==
             catalog::CatalogIdentifierKeyError::none;
    };
    // The entries that may hold a name: each stored entry row's newest version
    // not rolled back or dead, then this request's entries as placed.
    struct Holder {
      const catalog::CatalogNamePayload* payload;
      Hold state;
      u64 order;
    };
    std::vector<Holder> holders;
    for (const auto& [row, k] : newest_name) {
      const auto& s = stored[k];
      const auto* e = std::get_if<catalog::CatalogNameEntry>(&s.payload);
      if (!e) continue;
      if (s.state && InProgress(*s.state) && s.order != request.transaction_order) {
        holders.push_back({&s.payload, Hold::pending, s.order});
        continue;
      }
      if (s.deleted || e->lifecycle_state == catalog::CatalogNameLifecycle::dropped ||
          e->lifecycle_state == catalog::CatalogNameLifecycle::archived)
        continue;
      holders.push_back({&s.payload, Hold::held, s.order});
    }

    for (std::size_t i = 0; i < request.rows.size(); ++i) {
      const auto& want = request.rows[i];
      auto& relation = relations.at(want.role);
      const auto& head = heads.at(want.role);
      const auto& m = want.metadata;
      const auto& h = m.record.header;
      // A first version by this transaction.
      if ((!want.name && catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(m))) ||
          h.row_uuid.kind != UuidKind::row || h.row_uuid.value != want.row_uuid || h.deleted ||
          m.definition_version != 1 || m.authority_scope == catalog::CatalogAuthorityScope::cluster ||
          m.creator_transaction_uuid.kind != UuidKind::transaction ||
          m.creator_transaction_uuid.value != request.transaction_uuid ||
          m.creator_local_transaction_id != request.transaction_order)
        return refuse_row(E::invalid_insert, i);
      if (AheadOfRoot(m, *root.root)) return refuse_row(E::ahead_of_catalog_root, i);
      // New to the relation: no row or version with these UUIDs; no row
      // holding the object (a hold another transaction in progress has is a
      // write conflict: if it rolls back the object can be inserted).
      // SB_CATALOG_SINGLE_WRITER: checked against images read above and
      // written below with nothing held between; only one catalog writer per
      // database makes the check still true when the pages are written.
      std::optional<u64> blocker;
      bool held = false;
      for (const auto& leaf : relation.leaves)
        for (const auto& row : leaf.page.body.rows) {
          if (row.row_uuid.value == want.row_uuid || row.version_uuid == want.version_uuid ||
              row.row_uuid.value == want.version_uuid || row.version_uuid == want.row_uuid)
            return refuse_row(E::duplicate_row, i);
          const auto found = leaf.metadata.find(row.version_uuid);
          if (found == leaf.metadata.end() || found->second.record.header.object_uuid.value != h.object_uuid.value)
            continue;
          const auto state = hold(row);
          if (state == Hold::held) held = true;
          if (state == Hold::pending && !blocker) blocker = row.local_transaction_id;
        }
      if (held) return refuse_row(E::duplicate_object, i);
      if (blocker) {
        result.blocker_order = blocker;
        return refuse_row(E::write_conflict, i);
      }
      for (const auto& page : relation.tree)
        for (const auto& cell : page.page.cells)
          if (cell.key.row_uuid == want.row_uuid) return refuse_row(E::duplicate_row, i);

      // An entry: its keys, then the names it could collide with.
      // SB_CATALOG_SINGLE_WRITER: like the object check above, this holds only
      // while no other catalog writer runs between the read and the write.
      if (want.name && std::holds_alternative<catalog::CatalogNameEntry>(*want.name)) {
        const auto& x = std::get<catalog::CatalogNameEntry>(*want.name);
        const auto gx = group_of(x);
        if (!gx || !keys_agree(*want.name)) return refuse_row(E::invalid_name, i);
        const auto xv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(*want.name));
        std::optional<u64> name_blocker;
        bool name_held = false;
        for (const auto& holder : holders) {
          const auto& y = std::get<catalog::CatalogNameEntry>(*holder.payload);
          if (y.object_uuid.value == x.object_uuid.value) continue;  // an alias of the same object
          const auto yv = std::get<catalog::CatalogNameEntryView>(catalog::BorrowCatalogNamePayload(*holder.payload));
          if (catalog::CompareCatalogNameCollisionScope(xv, yv)) continue;
          const auto gy = group_of(y);
          if (!gy) {
            if (holder.state != Hold::pending) return Refuse(E::invalid_catalog);
            if (!name_blocker) name_blocker = holder.order;
            continue;
          }
          if (x.object_class == y.object_class && gy->body != gx->body) return refuse_row(E::invalid_name, i);
          if (gy->body != gx->body || gy->group != gx->group ||
              catalog::CompareCatalogNameCollisionLanes(xv, yv, gx->language, gx->key))
            continue;
          if (holder.state == Hold::held) name_held = true;
          else if (!name_blocker) name_blocker = holder.order;
        }
        if (name_held) return refuse_row(E::duplicate_name, i);
        if (name_blocker) {
          result.blocker_order = name_blocker;
          return refuse_row(E::write_conflict, i);
        }
        holders.push_back({&*want.name, Hold::held, request.transaction_order});
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
      std::optional<std::size_t> target;
      for (std::size_t l = 0; l < relation.leaves.size() && !target; ++l) {
        auto page = relation.leaves[l].page;
        u32 slot = 0;
        for (const auto& row : page.body.rows) slot = std::max(slot, row.stable_slot_id);
        if (slot == std::numeric_limits<u32>::max()) continue;
        auto final_row = placed(page, slot + 1);
        if (!final_row) return refuse_row(E::invalid_insert, i);
        auto row = added;
        row.stable_slot_id = slot + 1;
        row.storage_generation = page.header.page_generation;
        scratchbird::core::datatypes::DatatypeBinaryValue cell_value;
        cell_value.type_id = scratchbird::core::datatypes::CanonicalTypeId::binary;
        cell_value.payload = std::move(final_row->second);
        row.cells = {{1, std::move(cell_value), {}}};
        page.body.rows.push_back(std::move(row));
        const auto fits = pg::BuildRowDataPageBody(page.body, page.header.page_size_bytes - 32);
        if (!fits.ok()) {
          if (fits.diagnostic.diagnostic_code == "SB-ROW-DATA-PAGE-BODY-TOO-LARGE") continue;
          if (fits.resource_failure()) return Refuse(E::resource_exhausted);
          return refuse_row(E::invalid_insert, i);
        }
        relation.leaves[l].page = std::move(page);
        relation.leaves[l].metadata.emplace(want.version_uuid, std::move(final_row->first));
        target = l;
      }
      if (!target) return refuse_row(E::relation_full, i);
      changed_leaves.insert({want.role, *target});
      if (head.page_type == 6) continue;

      // The index: the leaf whose key range holds the key, the cell at its
      // position. With low-key separators nothing above the leaf changes.
      pg::NativeBtreeKey key;
      key.encoded_key.assign(want.row_uuid.bytes.begin(), want.row_uuid.bytes.end());
      key.row_uuid = want.row_uuid;
      key.version_uuid = want.version_uuid;
      const auto view = [](const pg::NativeBtreeKey& k) {
        return pg::NativeBtreeKeyView{std::span<const byte>(k.encoded_key), k.row_uuid, k.version_uuid};
      };
      std::optional<std::size_t> reached;
      for (std::size_t t = 0; t < relation.tree.size(); ++t) {
        const auto& page = relation.tree[t].page;
        if (page.tree_level) continue;
        if (page.low_fence && pg::CompareNativeBtreeKeys(view(key), view(*page.low_fence)) < 0) continue;
        if (page.high_fence && pg::CompareNativeBtreeKeys(view(key), view(*page.high_fence)) >= 0) continue;
        reached = t;
        break;
      }
      if (!reached) return refuse_row(E::invalid_catalog, i);
      auto page = relation.tree[*reached].page;
      const auto at = std::lower_bound(page.cells.begin(), page.cells.end(), key, [&](const auto& c, const auto& k) {
        return pg::CompareNativeBtreeKeys(view(c.key), view(k)) < 0;
      });
      pg::NativeBtreeCell cell;
      cell.key = key;
      cell.base_page = relation.bases[*target];
      page.cells.insert(at, std::move(cell));
      const auto fits = pg::EncodeNativeBtreePage(page);
      if (!fits.ok()) return refuse_row(fits.error == pg::NativeBtreeError::resource_exhausted ? E::index_full
                                                                                              : E::invalid_catalog,
                                        i);
      relation.tree[*reached].page = std::move(page);
      changed_tree.insert({want.role, *reached});
    }

    // ---- the images --------------------------------------------------------
    // Pages are kept apart (each image lives until the plan executes).
    struct Planned {
      u64 page_number;
      std::vector<byte> image;
      const std::vector<byte>* as_read;
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
      leaf_writes.push_back({relations.at(role).bases[l].page_number, std::move(encoded.bytes), &leaf.image});
    }
    for (const auto& [role, t] : changed_tree) {
      auto& page = relations.at(role).tree[t];
      auto encoded = pg::EncodeNativeBtreePage(page.page);
      if (!encoded.ok()) return Refuse(E::invalid_catalog);
      tree_writes.push_back({page.ref.page_number, std::move(encoded.bytes), &page.image});
    }

    // ---- the writes: every base leaf before every index leaf ----------------
    disk::CarefulWritePlan plan;
    std::vector<disk::CarefulWriteNodeId> leaf_nodes;
    for (const auto& w : leaf_writes) {
      const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(w.image),
                                 std::span<const byte>(*w.as_read));
      if (!node.ok()) return Refuse(E::plan_refused);
      leaf_nodes.push_back(node.node);
    }
    for (const auto& w : tree_writes) {
      const auto node = plan.Add(w.page_number * u64{page_size}, std::span<const byte>(w.image),
                                 std::span<const byte>(*w.as_read));
      if (!node.ok()) return Refuse(E::plan_refused);
      for (const auto before : leaf_nodes)
        if (plan.Before(before, node.node) != CarefulWriteError::none) return Refuse(E::plan_refused);
    }
    const auto r = plan.Execute(sink, binding.barrier_policy);
    result.write_error = r.error;
    result.io = r.io;
    result.writes = r.written;
    result.syncs = r.syncs;
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

}  // namespace scratchbird::storage::database
