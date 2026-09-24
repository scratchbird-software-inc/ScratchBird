// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real database file create/reopen in separate processes. Not SQL/IPC evidence.
#include "database_lifecycle.hpp"
#include "resource_seed_pack.hpp"
#include "unicode_normalization.hpp"
#include "unicode_collation.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "catalog_page.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/resource_catalog_admission.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
#include "local_transaction_store.hpp"
#include "transaction_inventory.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace db = scratchbird::storage::database;
namespace r = scratchbird::core::resources;
namespace uuid = scratchbird::core::uuid;
namespace memory = scratchbird::core::memory;
namespace fs = std::filesystem;
namespace page = scratchbird::storage::page;
namespace disk = scratchbird::storage::disk;
namespace platform = scratchbird::core::platform;
using scratchbird::core::platform::UuidKind;
namespace engine = scratchbird::engine::internal_api;
namespace mga = scratchbird::transaction::mga;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Good(const db::DatabaseLifecycleResult& result) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.message_key;
    for (const auto& arg : result.diagnostic.arguments) std::cerr << ' ' << arg.key << '=' << arg.value;
    std::cerr << '\n';
    throw std::runtime_error("database lifecycle failure");
  }
}
std::uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Quote(const std::string& value) {
  std::string result="'";
  for (char c:value) result += c=='\'' ? "'\\''" : std::string(1,c);
  return result+"'";
}
std::string IdentityBytes(const r::ResourceSeedCatalogImage& image) {
  std::string bytes;
  const auto append=[&](const platform::Uuid& id) {
    bytes.append(reinterpret_cast<const char*>(id.bytes.data()),id.bytes.size());
  };
  for (const auto& row:image.charsets) { append(row.resource_uuid);append(row.default_collation_uuid); }
  for (const auto& row:image.collations) { append(row.resource_uuid);append(row.charset_uuid); }
  for (const auto& row:image.timezones) append(row.resource_uuid);
  for (const auto& row:image.aliases) append(row.canonical_resource_uuid);
  for (const auto& row:image.artifacts) append(row.artifact_uuid);
  return bytes;
}
void BehaviorReadFailures(const fs::path& path) {
  // Exercise the production loader and both production selection APIs against
  // actual storage. This does not certify the legacy record format or SQL/IPC.
  engine::EngineRequestContext context;
  context.database_path = path.string();
  const fs::path journal = path.string() + ".sb.api_events.v2";
  Require(!fs::exists(journal), "behavior journal fixture must be isolated");
  unsigned checks = 0;
  const auto check = [&](bool ok, const char* detail) { ++checks; Require(ok, detail); };
  const auto probe = [&](const engine::EngineRequestContext& candidate, bool success,
                         const char* detail, bool native_cause = false) {
    const auto loaded = engine::LoadApiBehaviorState(candidate);
    check(loaded.ok == success && loaded.diagnostic.error == !success &&
          loaded.state.records.empty(), "behavior load outcome or empty state incorrect");
    engine::EngineApiDiagnostic diagnostic;
    diagnostic.error = true; diagnostic.code = "fixture_prior_error";
    const auto rows = engine::VisibleApiBehaviorRecords(candidate, {}, 0, diagnostic);
    check(rows.empty() && diagnostic.error == !success &&
          diagnostic.code == loaded.diagnostic.code &&
          diagnostic.message_key == loaded.diagnostic.message_key &&
          diagnostic.detail == loaded.diagnostic.detail,
          "behavior list lost failure or retained an obsolete diagnostic");
    diagnostic.error = true; diagnostic.code = "fixture_prior_error";
    const auto found = engine::FindVisibleApiBehaviorRecord(candidate, {}, 0, diagnostic);
    check(!found && diagnostic.error == !success &&
          diagnostic.code == loaded.diagnostic.code &&
          diagnostic.message_key == loaded.diagnostic.message_key &&
          diagnostic.detail == loaded.diagnostic.detail,
          "behavior lookup changed read failure into ordinary absence");
    if (detail) check(loaded.diagnostic.detail == std::string("api_behavior.load_state:") + detail,
                      "behavior read failure detail mismatch");
    if (native_cause) {
      const auto native = db::LoadLocalTransactionInventoryFromDatabase(candidate.database_path);
      check(!native.ok() && loaded.diagnostic.native_source.has_value() &&
            diagnostic.native_source.has_value(), "native inventory failure was discarded");
      check(loaded.diagnostic.native_source->record.diagnostic_code == native.diagnostic.diagnostic_code &&
            loaded.diagnostic.native_source->record.message_key == native.diagnostic.message_key &&
            diagnostic.native_source->record.diagnostic_code == native.diagnostic.diagnostic_code &&
            diagnostic.native_source->record.message_key == native.diagnostic.message_key,
            "native inventory failure was rewritten at behavior selection");
    }
  };
  probe(context, true, nullptr); // Real database, no behavior records.
  { std::ofstream out(journal, std::ios::binary); Require(out.good(), "empty journal create failed"); }
  probe(context, true, nullptr);
  Require(fs::remove(journal), "empty journal cleanup failed");
  const auto object = uuid::GenerateEngineIdentityV7(UuidKind::object, Now());
  Require(object.ok(), "behavior binary fixture object identity");
  engine::ApiBehaviorRecord record;
  record.object_uuid = object.value.value;
  record.operation_id = "behavior_read_fixture";
  record.object_kind = "object";
  record.state = "active";
  record.payload = std::string("binary\0payload\n|", 16);
  std::string frame;
  Require(engine::EncodeApiBehaviorRecord(record, &frame), "native behavior frame encoding");
  Require(!frame.empty(), "native behavior frame encoding failed");
  const auto malformed = [&](const std::string& bytes) {
    { std::ofstream out(journal, std::ios::binary); out.write(bytes.data(), bytes.size());
      Require(out.good(), "malformed journal fixture write failed"); }
    probe(context, false, "api_journal_binary_record_invalid");
    Require(fs::remove(journal), "malformed journal cleanup failed");
  };
  // Every nonempty truncated prefix must fail without exposing partial state.
  for (std::size_t size = 1; size < frame.size(); ++size)
    malformed(frame.substr(0, size));
  malformed("SBAPI1\tRECORD\tlegacy-text-identity\n");
  malformed(frame + "trailing");
  const auto checksum = [&](std::string bytes) {
    const auto hash = scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const std::uint8_t*>(bytes.data() + 4), bytes.size() - 36);
    Require(hash.ok(), "hostile behavior fixture checksum");
    std::copy(hash.digest.begin(), hash.digest.end(), bytes.end() - 32);
    return bytes;
  };
  auto corrupt = frame; corrupt.back() ^= 1; malformed(corrupt);
  corrupt = frame; corrupt[4] ^= 1; malformed(checksum(corrupt));
  for (unsigned identity = 0; identity < 4; ++identity) {
    corrupt = frame; corrupt[20 + identity * 16 + 6] = 0x40;
    malformed(checksum(corrupt));
    corrupt = frame; corrupt[20 + identity * 16 + 8] = 0x00;
    // Optional nil UUIDs are valid; make the malformed identity nonnil.
    corrupt[20 + identity * 16] = 1;
    malformed(checksum(corrupt));
  }
  corrupt = frame; std::fill_n(corrupt.begin() + 20, 16, '\0');
  malformed(checksum(corrupt));
  corrupt = frame; corrupt[corrupt.size() - 33] = 2;
  malformed(checksum(corrupt));
  corrupt = frame; std::fill_n(corrupt.begin() + 84, 4, static_cast<char>(0xff));
  malformed(checksum(corrupt));
  const fs::path legacy = path.string() + ".sb.api_events";
  { std::ofstream out(legacy, std::ios::binary); out << "SBAPI1\tRECORD\n";
    Require(out.good(), "legacy refusal fixture write failed"); }
  probe(context, false, "legacy_format_unsupported");
  Require(fs::remove(legacy), "legacy refusal fixture cleanup failed");
  Require(fs::create_directory(journal), "directory journal create failed");
  probe(context, false, "api_journal_not_regular");
  Require(fs::remove(journal), "directory journal cleanup failed");
  fs::create_symlink(path.filename(), journal);
  probe(context, false, "api_journal_symlink_forbidden");
  Require(fs::remove(journal), "journal symlink cleanup failed");
  fs::create_symlink("absent-journal-target", journal);
  probe(context, false, "api_journal_symlink_forbidden");
  Require(fs::remove(journal), "dangling journal symlink cleanup failed");
  auto bad = context; bad.database_path.clear();
  probe(bad, false, "database_path_required");
  bad.database_path = (path.parent_path() / "missing-behavior.sbdb").string();
  probe(bad, false, nullptr, true); // No journal cannot make a missing node valid.
  bad.database_path = (path.parent_path() / "invalid-behavior.sbdb").string();
  { std::ofstream out(bad.database_path, std::ios::binary); out << "not a database";
    Require(out.good(), "invalid database fixture create failed"); }
  probe(bad, false, nullptr, true);
  Require(fs::remove(bad.database_path), "invalid database fixture cleanup failed");
  probe(context, true, nullptr); // A prior failed lookup cannot poison a valid empty read.
  // Native journal visibility follows actual durable archived MGA origins.
  for (const bool commit : {false, true}) {
    const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(path.string());
    Require(loaded.ok(), "load native archive visibility fixture");
    const auto id = uuid::GenerateEngineIdentityV7(UuidKind::transaction, Now());
    Require(id.ok(), "archive visibility transaction UUID");
    const auto begun = mga::BeginLocalTransaction(loaded.inventory, id.value, Now());
    Require(begun.ok(), "begin native archive visibility fixture");
    const auto finalized = commit ? mga::CommitLocalTransaction(begun.inventory, begun.entry.identity.local_id, Now()) :
        mga::RollbackLocalTransaction(begun.inventory, begun.entry.identity.local_id, Now());
    Require(finalized.ok(), "finalize native archive visibility fixture");
    const auto archived = mga::ArchiveLocalTransaction(finalized.inventory, begun.entry.identity.local_id);
    Require(archived.ok(), "archive native visibility fixture");
    const auto persisted = db::PersistLocalTransactionInventoryToDatabase(path.string(), archived.inventory);
    Require(persisted.ok(), "persist native archive visibility fixture");
    {
      std::ofstream out(journal, std::ios::binary);
      record.creator_tx = begun.entry.identity.local_id.value;
      std::string bytes;
      Require(engine::EncodeApiBehaviorRecord(record, &bytes), "native archive frame encoding");
      Require(!bytes.empty(), "native archive visibility record encoding failed");
      out.write(bytes.data(), bytes.size());
      out.close(); Require(out.good(), "write native archive visibility fixture");
    }
    const auto visible = engine::LoadApiBehaviorState(context);
    check(visible.ok && visible.state.records.size() == (commit ? 1u : 0u),
        "archived terminal origin lost in engine metadata visibility");
    if (commit) {
      check(visible.state.records.front().object_uuid == record.object_uuid &&
                visible.state.records.front().payload == record.payload,
            "native journal changed binary identity or embedded payload bytes");
    }
    Require(fs::remove(journal), "remove native visibility fixture");
  }
  std::cout << "PASS behavior storage read failure propagation checks=" << checks << '\n';
}
void ResourceAdmission(const fs::path& path) {
  db::DatabaseOpenConfig open; open.path=path.string(); open.read_only=true;
  open.suppress_background_agents=true;
  const auto initial=db::OpenDatabaseFile(open); Good(initial);
  const auto& image=initial.state.resource_seed_catalog;
  Require(!image.charsets.empty() && !image.collations.empty(),"resource descriptor oracle missing");
  engine::EngineRequestContext context;
  context.database_path=path.string(); context.database_uuid=initial.state.database_uuid.value;
  context.resource_epoch=image.resource_epoch;
  auto inventory=initial.state.local_transaction_inventory;
  const auto begin=[&](bool read_only) {
    const auto id=uuid::GenerateEngineIdentityV7(UuidKind::transaction,Now());
    Require(id.ok(),"resource admission transaction identity failed");
    const auto begun=read_only
        ? mga::BeginLocalReadOnlyTransaction(inventory,id.value,Now())
        : mga::BeginLocalTransaction(inventory,id.value,Now());
    Require(begun.ok(),"resource admission actual transaction begin failed");
    const auto persisted=db::PersistLocalTransactionInventoryToDatabase(path.string(),begun.inventory);
    Require(persisted.ok(),"resource admission transaction persistence failed");
    inventory=persisted.inventory;
    context.transaction_uuid=id.value.value;
    context.local_transaction_id=begun.entry.identity.local_id.value;
  };
  begin(false);
  unsigned checks=0;
  const auto check=[&](bool condition,const char* detail){++checks;Require(condition,detail);};
  const auto lookup=[&](const engine::EngineRequestContext& candidate) {
    return engine::LookupEngineResourceDescriptorByUuid(candidate,image.charsets.front().resource_uuid,"charset");
  };
  const auto refused=[&](const engine::EngineRequestContext& candidate,const char* key) {
    const auto resource=lookup(candidate);
    check(!resource.ok && resource.diagnostic.error && resource.diagnostic.code=="CATALOG.INVALID_INPUT" && resource.diagnostic.message_key==key &&
          resource.diagnostic.canonical_metadata.has_value() &&
          !resource.resource_descriptor.present && resource.resource_descriptor.resource_uuid.is_nil(),
          "resource lookup published authority for invalid context");
    const auto named=engine::LookupEngineResourceDescriptorByName(
        candidate,image.charsets.front().canonical_name,"charset");
    check(!named.ok && named.diagnostic.error && named.diagnostic.code=="CATALOG.INVALID_INPUT" &&
          named.diagnostic.message_key==key && named.diagnostic.canonical_metadata.has_value() &&
          !named.resource_descriptor.present && named.resource_descriptor.resource_uuid.is_nil(),
          "name lookup bypassed node transaction or resource cohort admission");
    const auto timezone=engine::LookupEngineTimezoneSeedAuthority(candidate);
    check(!timezone.ok && timezone.diagnostic.error && timezone.diagnostic.code=="CATALOG.INVALID_INPUT" && timezone.diagnostic.message_key==key &&
          timezone.diagnostic.canonical_metadata.has_value() &&
          !timezone.authority.active && timezone.authority.timezone_names.empty(),
          "timezone lookup published authority for invalid context");
  };
  const auto charset=lookup(context);
  check(charset.ok && charset.resource_descriptor.resource_uuid==image.charsets.front().resource_uuid &&
        charset.resource_descriptor.default_collation_uuid==image.charsets.front().default_collation_uuid &&
        charset.resource_descriptor.family_epoch==image.charsets.front().family_epoch &&
        charset.resource_descriptor.canonical_name==image.charsets.front().canonical_name,
        "actual charset lookup changed binary identity or cohort");
  const auto collation=engine::LookupEngineResourceDescriptorByUuid(
      context,image.collations.front().resource_uuid,"COLLATION");
  check(collation.ok && collation.resource_descriptor.resource_uuid==image.collations.front().resource_uuid &&
        collation.resource_descriptor.database_uuid==context.database_uuid &&
        collation.resource_descriptor.comparison_profile==image.collations.front().comparison_profile &&
        collation.resource_descriptor.parent_resource_uuid==image.collations.front().charset_uuid &&
        collation.resource_descriptor.family_version==image.collations.front().family_version,
        "actual collation lookup changed binary identity or parent");
  unsigned executable_profiles=0;
  for(const auto& row:image.collations) {
    if(row.comparison_profile==r::CollationProfile::unbound)continue;
    ++executable_profiles;
    const auto found=engine::LookupEngineResourceDescriptorByUuid(context,row.resource_uuid,"collation");
    check(found.ok && found.resource_descriptor.database_uuid==context.database_uuid &&
        found.resource_descriptor.resource_uuid==row.resource_uuid &&
        found.resource_descriptor.parent_resource_uuid==row.charset_uuid &&
        found.resource_descriptor.resource_epoch==image.resource_epoch &&
        found.resource_descriptor.family_epoch==row.family_epoch &&
        found.resource_descriptor.comparison_profile==row.comparison_profile,
        "live binary lookup lost comparison profile or owning cohort");
    std::string folded_name=row.canonical_name;
    for(auto& ch:folded_name)if(ch>='A'&&ch<='Z')ch+=('a'-'A');
    const auto named=engine::LookupEngineResourceDescriptorByName(context,folded_name,"COLLATION");
    const auto& n=named.resource_descriptor;
    const auto& d=found.resource_descriptor;
    check(named.ok && n.present && n.database_uuid==d.database_uuid &&
          n.resource_uuid==d.resource_uuid && n.parent_resource_uuid==d.parent_resource_uuid &&
          n.default_collation_uuid==d.default_collation_uuid && n.canonical_name==d.canonical_name &&
          n.resource_family==d.resource_family && n.parent_canonical_name==d.parent_canonical_name &&
          n.default_collation_name==d.default_collation_name && n.seed_pack_name==d.seed_pack_name &&
          n.seed_pack_version==d.seed_pack_version && n.resource_epoch==d.resource_epoch &&
          n.family_epoch==d.family_epoch && n.family_version==d.family_version &&
          n.min_bytes==d.min_bytes && n.max_bytes==d.max_bytes && n.variable_width==d.variable_width &&
          n.default_for_parent==d.default_for_parent && n.case_insensitive==d.case_insensitive &&
          n.accent_insensitive==d.accent_insensitive && n.comparison_profile==d.comparison_profile &&
          bool(n.unicode_collation)==bool(d.unicode_collation),
          "name resolution and binary lookup published different resource cohorts");
    const bool root=r::UsesUnicodeRoot(row.comparison_profile);
    namespace dt=scratchbird::core::datatypes;
    const auto seed=engine::TextSeedFromResource(named.resource_descriptor);
    dt::DatatypeComparisonRequest compare;
    compare.left={dt::CanonicalTypeId::character,"\xc3\xa9",false};
    compare.right={dt::CanonicalTypeId::character,"e\xcc\x81",false};compare.text_seed=seed;
    const auto compared=dt::CompareDatatypeValues(compare);
    check(compared.ok() && (compared.comparison==0)==root,
        "actual resource lookup to datatype comparison lost profile semantics");
    dt::DatatypeSortKeyRequest sort;sort.text_seed=seed;sort.value=compare.left;
    const auto first=dt::MakeDatatypeSortKey(sort);sort.value=compare.right;
    const auto second=dt::MakeDatatypeSortKey(sort);
    check(first.ok()&&second.ok()&&(first.sort_key==second.sort_key)==root,
        "actual resource lookup to sort key lost canonical equivalence");
    dt::DatatypeHashRequest hash;hash.text_seed=seed;hash.value=compare.left;
    const auto first_hash=dt::HashDatatypeValue(hash);hash.value=compare.right;
    const auto second_hash=dt::HashDatatypeValue(hash);
    check(first_hash.ok()&&second_hash.ok()&&(!root||first_hash.stable_hash_hex==second_hash.stable_hash_hex),
        "actual resource lookup produced inconsistent text hash");
    check(bool(found.resource_descriptor.unicode_collation)==root,
        "resolved profile has missing or extraneous root authority");
    if(root) {
      int order=99;
      const auto strength=static_cast<r::UnicodeCollationStrength>(
          static_cast<std::uint64_t>(row.comparison_profile)-1);
      check(found.resource_descriptor.unicode_collation->Compare("\xc3\xa9","e\xcc\x81",strength,
          {64,64},&order)==r::UnicodeNormalizationStatus::ok && order==0,
          "resolved database-owned recipe failed canonical equivalence");
    }
  }
  check(executable_profiles==5,"native comparison recipes missing from live catalog");
  const auto charset_name=engine::LookupEngineResourceDescriptorByName(
      context,image.charsets.front().canonical_name,"charset");
  check(charset_name.ok && charset_name.resource_descriptor.database_uuid==context.database_uuid &&
        charset_name.resource_descriptor.resource_uuid==charset.resource_descriptor.resource_uuid &&
        charset_name.resource_descriptor.default_collation_uuid==charset.resource_descriptor.default_collation_uuid &&
        charset_name.resource_descriptor.min_bytes==charset.resource_descriptor.min_bytes &&
        charset_name.resource_descriptor.max_bytes==charset.resource_descriptor.max_bytes &&
        charset_name.resource_descriptor.variable_width==charset.resource_descriptor.variable_width &&
        !charset_name.resource_descriptor.unicode_collation,
        "charset name lookup lost its exact descriptor");
  const auto alias_name=engine::LookupEngineResourceDescriptorByName(context,"cp936","charset");
  const auto* gbk=r::FindResourceSeedCharset(image,"GBK");
  check(gbk && alias_name.ok && alias_name.resource_descriptor.resource_uuid==gbk->resource_uuid &&
        alias_name.resource_descriptor.database_uuid==context.database_uuid,
        "charset alias did not resolve the database-owned binary identity");
  const auto ambiguous_name=engine::LookupEngineResourceDescriptorByName(context,"GB2312","charset");
  check(!ambiguous_name.ok && ambiguous_name.diagnostic.code=="SB_RESOURCE_ALIAS_AMBIGUOUS" &&
        ambiguous_name.diagnostic.canonical_metadata.has_value() && !ambiguous_name.resource_descriptor.present,
        "ambiguous resource alias published a descriptor");
  for(const auto& invalid:std::vector<std::string>{"",std::string("UTF\0-8",6),"\xc0\xaf","\xed\xa0\x80"}) {
    const auto bad_name=engine::LookupEngineResourceDescriptorByName(context,invalid,"charset");
    check(!bad_name.ok && bad_name.diagnostic.code=="CATALOG.INVALID_INPUT" &&
          bad_name.diagnostic.message_key=="catalog.resource.name_invalid" &&
          !bad_name.resource_descriptor.present,"invalid UTF8 resource name was admitted");
  }
  for(const auto& family:std::vector<std::string>{"charset","collation"}) {
    const auto absent_name=engine::LookupEngineResourceDescriptorByName(context,"__missing_resource__",family);
    check(!absent_name.ok && absent_name.diagnostic.code=="CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE" &&
          !absent_name.resource_descriptor.present,"unknown name fell back to a default resource");
  }
  const auto bad_family=engine::LookupEngineResourceDescriptorByName(context,"UTF-8","not_a_family");
  check(!bad_family.ok && bad_family.diagnostic.message_key=="catalog.resource.family_invalid" &&
        !bad_family.resource_descriptor.present,"unknown name family was admitted");
  const auto zone=engine::LookupEngineTimezoneSeedAuthority(context);
  check(zone.ok && zone.authority.active && zone.authority.timezone_epoch==image.timezone_epoch &&
        zone.authority.timezone_records==image.timezone_records,
        "actual timezone lookup rejected exact active transaction");
  auto bad=context; bad.database_uuid={}; refused(bad,"catalog.resource.database_required");
  bad=context; bad.database_path.clear(); refused(bad,"catalog.resource.database_required");
  bad=context; bad.database_uuid.bytes[6]=0x40;
  refused(bad,"catalog.resource.database_identity_mismatch");
  bad=context; bad.database_uuid.bytes[15]^=1;
  refused(bad,"catalog.resource.database_identity_mismatch");
  bad=context; bad.transaction_uuid={}; refused(bad,"catalog.resource.transaction_required");
  bad=context; bad.local_transaction_id=0; refused(bad,"catalog.resource.transaction_required");
  for (unsigned version=1;version<=6;++version) {
    bad=context; bad.transaction_uuid.bytes[6]=static_cast<unsigned char>(version<<4);
    refused(bad,"catalog.resource.transaction_invalid");
    auto identity=image.charsets.front().resource_uuid;
    identity.bytes[6]=static_cast<unsigned char>(version<<4);
    const auto result=engine::LookupEngineResourceDescriptorByUuid(context,identity,"charset");
    check(!result.ok && result.diagnostic.code=="CATALOG.INVALID_INPUT" && result.diagnostic.message_key=="catalog.resource.uuid_invalid" &&
          result.diagnostic.canonical_metadata.has_value() &&
          !result.resource_descriptor.present,"old UUID version admitted as system resource identity");
  }
  bad=context; bad.transaction_uuid.bytes[15]^=1;
  refused(bad,"catalog.resource.transaction_not_active");
  bad=context; bad.local_transaction_id+=100;
  refused(bad,"catalog.resource.transaction_not_active");
  bad=context; bad.resource_epoch=0; refused(bad,"catalog.resource.epoch_required");
  bad=context; ++bad.resource_epoch; refused(bad,"catalog.resource.epoch_stale");
  const auto wrong_family=engine::LookupEngineResourceDescriptorByUuid(
      context,image.collations.front().resource_uuid,"charset");
  check(!wrong_family.ok && wrong_family.diagnostic.code=="CATALOG.INVALID_INPUT" &&
        wrong_family.diagnostic.message_key=="catalog.resource.family_mismatch" &&
        wrong_family.diagnostic.canonical_metadata.has_value() &&
        !wrong_family.resource_descriptor.present,"resource family mismatch was not refused");
  const auto invalid_resource=[&](platform::Uuid id,const char* family,const char* key) {
    const auto result=engine::LookupEngineResourceDescriptorByUuid(context,id,family);
    check(!result.ok && result.diagnostic.code=="CATALOG.INVALID_INPUT" &&
          result.diagnostic.message_key==key && result.diagnostic.canonical_metadata.has_value() &&
          !result.resource_descriptor.present && result.resource_descriptor.resource_uuid.is_nil(),
          "invalid resource identity/family published a descriptor");
  };
  invalid_resource({},"charset","catalog.resource.uuid_required");
  invalid_resource(image.charsets.front().resource_uuid,"unknown","catalog.resource.family_invalid");
  auto invalid_id=image.charsets.front().resource_uuid; invalid_id.bytes[8]&=0x3f;
  invalid_resource(invalid_id,"charset","catalog.resource.uuid_invalid");
  const auto unknown_id=uuid::GenerateEngineIdentityV7(UuidKind::object,Now());
  Require(unknown_id.ok(),"absent resource fixture identity generation failed");
  const auto absent=engine::LookupEngineResourceDescriptorByUuid(context,unknown_id.value.value,"charset");
  check(!absent.ok && absent.diagnostic.code=="CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE" &&
        absent.diagnostic.canonical_metadata.has_value() && !absent.resource_descriptor.present,
        "absent resource used a name fallback or unregistered diagnostic");
  bad=context; bad.database_path=(path.parent_path()/"missing.sbdb").string();
  const auto missing=lookup(bad);
  auto missing_config=open; missing_config.path=bad.database_path;
  const auto native_missing=db::OpenDatabaseFile(missing_config);
  check(!missing.ok && missing.diagnostic.native_source.has_value() && missing.diagnostic.canonical_metadata.has_value() &&
        !native_missing.ok() && missing.diagnostic.native_source->record.diagnostic_code==
            native_missing.diagnostic.diagnostic_code &&
        missing.diagnostic.native_source->record.message_key==native_missing.diagnostic.message_key &&
        !missing.resource_descriptor.present,"native storage failure cause was discarded");
  const auto finalize=[&] {
    const auto committed=mga::CommitLocalTransaction(inventory,{context.local_transaction_id},Now());
    Require(committed.ok(),"resource admission actual transaction commit failed");
    const auto persisted=db::PersistLocalTransactionInventoryToDatabase(path.string(),committed.inventory);
    Require(persisted.ok(),"resource admission commit persistence failed");
    inventory=persisted.inventory;
    refused(context,"catalog.resource.transaction_not_active");
  };
  finalize();
  begin(true);
  check(lookup(context).ok,"read-only-active transaction could not admit resource");
  check(engine::LookupEngineTimezoneSeedAuthority(context).ok,"read-only-active timezone admission failed");
  finalize();
  std::cout << "PASS actual resource engine admission checks=" << checks << '\n';
}
struct SavedPage { std::uint64_t number; std::uint32_t page_size; std::vector<platform::byte> body; };
void WritePage(const fs::path& path,const SavedPage& page) {
  disk::FileDevice device;
  Require(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"fixture write open failed");
  Require(device.WriteAt(page.number*page.page_size+disk::kPageHeaderSerializedBytes,
                        page.body.data(),page.body.size()).ok() && device.Sync().ok(),"fixture write failed");
}
enum class Corruption { artifact, cycle, alias_identity, alias_epoch };
SavedPage Corrupt(const fs::path& path,Corruption corruption) {
  SavedPage original{}; SavedPage changed{};
  {
    disk::FileDevice device;
    Require(device.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"fixture read failed");
    disk::SerializedDatabaseHeader header{};
    Require(device.ReadAt(0,header.data(),header.size()).ok(),"fixture header read failed");
    const auto parsed=disk::ParseDatabaseHeader(header); Require(parsed.ok(),"fixture header invalid");
    std::uint64_t number=db::kCatalogPageNumber;
    while (number) {
      original={number,parsed.header.page_size,{}};
      original.body.resize(original.page_size-disk::kPageHeaderSerializedBytes);
      Require(device.ReadAt(number*original.page_size+disk::kPageHeaderSerializedBytes,
                            original.body.data(),original.body.size()).ok(),"fixture page read failed");
      const auto body=page::ParseCatalogPageBody(original.body,number); Require(body.ok(),"fixture catalog invalid");
      changed=original;
      if (corruption==Corruption::cycle) { platform::StoreLittle64(changed.body.data()+32,number); break; }
      std::size_t offset=page::kCatalogPageBodyHeaderBytes;
      bool selected=false;
      for (const auto& row:body.body.rows) {
        if ((corruption==Corruption::artifact && row.kind==page::CatalogPageRowKind::resource_seed_artifact) ||
            (corruption!=Corruption::artifact && row.kind==page::CatalogPageRowKind::charset_alias_record)) {
          if (corruption==Corruption::artifact) {
            Require(row.payload.size()>48 && row.payload.substr(0,4)=="RSAC","fixture artifact has wrong layout");
            changed.body[offset+20+row.payload.size()-1]^=1;
          } else {
            Require(row.payload.size()>24 && row.payload.substr(0,4)=="SBCV","fixture alias has wrong layout");
            bool field_found=false;
            for (std::size_t pos=24;pos+8<=row.payload.size();) {
              const auto* bytes=reinterpret_cast<const platform::byte*>(row.payload.data()+pos);
              const auto id=platform::LoadLittle16(bytes);
              const auto length=platform::LoadLittle32(bytes+4);
              Require(length<=row.payload.size()-pos-8,"fixture alias field out of bounds");
              if ((corruption==Corruption::alias_identity && id==4) ||
                  (corruption==Corruption::alias_epoch && id==6)) {
                if (id==4) { Require(length==16,"alias identity is not binary16"); changed.body[offset+20+pos+8+6]=0x40; }
                else { Require(length==8,"alias epoch is not u64"); changed.body[offset+20+pos+8]^=2; }
                field_found=true;break;
              }
              pos+=8+length;
            }
            Require(field_found,"fixture alias target field missing");
          }
          std::uint64_t hash=1469598103934665603ULL;
          for (std::size_t i=0;i<row.payload.size();++i) {
            hash^=changed.body[offset+20+i]; hash*=1099511628211ULL;
          }
          platform::StoreLittle64(changed.body.data()+offset+12,hash);
          selected=true; break;
        }
        offset+=20+row.payload.size();
      }
      if (selected) break;
      number=body.body.next_page_number;
    }
    Require(number!=0,"fixture artifact page not found");
  }
  platform::StoreLittle64(changed.body.data()+40,page::ComputeCatalogPageBodyChecksum(changed.body));
  Require(page::ParseCatalogPageBody(changed.body,changed.number).ok(),"fixture outer checksums not valid");
  WritePage(path,changed); return original;
}
int main(int argc,char** argv) {
  try {
    if (argc==3) {
      auto policy=memory::DefaultLocalEngineMemoryPolicy(); policy.policy_name="resource_content_lifecycle";
      Require(memory::ConfigureDefaultMemoryManagerForFixture(policy,"resource_content_lifecycle").ok(),
              "memory fixture configuration failed");
      const fs::path root=argv[2];
      if (std::string_view(argv[1])=="--create") {
        const auto now=Now();
        const auto database=uuid::GenerateEngineIdentityV7(UuidKind::database,now);
        const auto filespace=uuid::GenerateEngineIdentityV7(UuidKind::filespace,now+1);
        Require(database.ok() && filespace.ok(),"fixture UUID generation failed");
        db::DatabaseCreateConfig config;
        config.path=(root/"content.sbdb").string(); config.database_uuid=database.value;
        config.filespace_uuid=filespace.value; config.creation_unix_epoch_millis=now;
        config.resource_seed_pack_root=(root/"initial-resource-pack").string();
        config.require_resource_seed_pack=true;
        const auto created=db::CreateDatabaseFile(config); Good(created);
        Require(!created.state.resource_seed_catalog.artifacts.empty(),"create did not retain artifacts");
        const auto ids=IdentityBytes(created.state.resource_seed_catalog);
        std::ofstream identity_file(root/"identity-oracle.bin",std::ios::binary);
        identity_file.write(ids.data(),ids.size());identity_file.close();
        Require(identity_file.good(),"independent identity oracle write failed");
        return 0;
      }
      const std::string_view mode=argv[1];
      Require(mode=="--reopen" || mode=="--admission" || mode=="--behavior-read" || mode=="--refuse-artifact" || mode=="--refuse-chain","unknown fixture mode");
      Require(!fs::exists(root/"initial-resource-pack"),"seed directory still present during reopen");
      if(mode=="--admission") { ResourceAdmission(root/"content.sbdb"); return 0; }
      if(mode=="--behavior-read") { BehaviorReadFailures(root/"content.sbdb"); return 0; }
      db::DatabaseOpenConfig config; config.path=(root/"content.sbdb").string();
      config.read_only=true; config.suppress_background_agents=true;
      const auto opened=db::OpenDatabaseFile(config);
      if (mode!="--reopen") {
        Require(!opened.ok() && opened.diagnostic.diagnostic_code ==
            (mode=="--refuse-artifact" ? "SB-CATALOG-RECORD-CODEC-FIELDS-MISSING" :
                                       "SB-CATALOG-PAGE-BODY-NEXT-CHAIN-TOO-LONG"),
                "corrupt resource database was not refused at expected boundary");
        std::cout << "PASS independent corruption refusal " << mode << '\n'; return 0;
      }
      Good(opened);
      r::ResourceSeedLoadConfig source; source.seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;
      const auto oracle=r::LoadResourceSeedPack(source); Require(oracle.ok(),"independent oracle pack failed");
      const auto& actual=opened.state.resource_seed_catalog;
      std::ifstream identity_file(root/"identity-oracle.bin",std::ios::binary);
      Require(identity_file.good(),"independent identity oracle read failed");
      const std::string ids(std::istreambuf_iterator<char>(identity_file),{});
      Require(IdentityBytes(actual)==ids,"binary resource or alias identities changed across process restart");
      for (const auto& row:actual.aliases) {
        Require(!row.canonical_resource_uuid.is_nil() && (row.canonical_resource_uuid.bytes[6]>>4)==7,
                "bound alias lost its binary UUIDv7 target");
      }
      const auto ambiguous=r::ResolveResourceSeedAlias(actual,r::ResourceSeedFamily::charset,"gb2312");
      Require(!ambiguous.ok() && ambiguous.diagnostic.diagnostic_code=="SB_RESOURCE_ALIAS_AMBIGUOUS" &&
              ambiguous.alias.canonical_resource_uuid.is_nil(),"persisted alias ambiguity changed");
      Require(actual.artifacts.size()==oracle.image.artifacts.size(),"reopen artifact count changed");
      Require(actual.unicode_normalization != nullptr, "reopen lost database-owned normalization table");
      Require(actual.unicode_collation != nullptr, "reopen lost database-owned UCA table");
      const std::array<const char*,5> profile_names{{"SB_UTF8_BINARY","SB_UCA_17_PRIMARY",
          "SB_UCA_17_SECONDARY","SB_UCA_17_TERTIARY","SB_UCA_17_IDENTICAL"}};
      for(std::size_t profile=0;profile<profile_names.size();++profile) {
        const auto* row=r::FindResourceSeedCollation(actual,profile_names[profile]);
        Require(row && static_cast<std::uint64_t>(row->comparison_profile)==profile+1,
                "reopen changed exact native numeric recipe");
      }
      const auto* donor=r::FindResourceSeedCollation(actual,"UNICODE_CI_AI");
      Require(donor && donor->comparison_profile==r::CollationProfile::unbound,
              "reopen silently mapped donor collation to root UCA");
      int unicode_comparison = 99;
      Require(actual.unicode_collation->Compare("\xce\x91", "\xce\xb1", r::UnicodeCollationStrength::primary,
                  {64,64}, &unicode_comparison) == r::UnicodeNormalizationStatus::ok && unicode_comparison == 0,
              "database-contained UCA Greek comparison after seed removal failed");
      std::string normalized;
      Require(actual.unicode_normalization->NormalizeNfd("\xe1\xb9\xa9\xea\xb0\x81", 14, &normalized) ==
                  r::UnicodeNormalizationStatus::ok &&
              normalized == "s\xcc\xa3\xcc\x87\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8",
              "database-contained canonical decomposition after seed removal failed");
      std::size_t bytes=0;
      for (std::size_t i=0;i<actual.artifacts.size();++i) {
        const auto& a=actual.artifacts[i]; const auto& e=oracle.image.artifacts[i];
        Require(!a.artifact_uuid.is_nil() && (a.artifact_uuid.bytes[6]>>4)==7,
                "persisted artifact identity is not binary system UUIDv7");
        Require(a.family==e.family && a.canonical_path==e.canonical_path && a.content && e.content &&
                *a.content==*e.content && a.content_hash==e.content_hash &&
                a.content_size_bytes==e.content_size_bytes,"reopen lost or changed resource content");
        bytes+=a.content->size();
      }
      std::cout << "PASS independent resource reopen artifacts=" << actual.artifacts.size()
                << " bytes=" << bytes << " timezone_identities=" << actual.timezones.size() << '\n';
      return 0;
    }
    Require(argc==1,"unexpected arguments");
    const auto id=uuid::GenerateEngineIdentityV7(UuidKind::object,Now()); Require(id.ok(),"temp identity failed");
    const auto root=fs::temp_directory_path()/("sb-resource-content-"+uuid::UuidToString(id.value.value));
    Require(fs::create_directory(root),"fixture directory already exists");
    struct Cleanup { fs::path root; ~Cleanup() {
      std::error_code error; const auto removed=fs::remove_all(root,error);
      std::cout << "resource_content_fixture_cleanup entries=" << removed << " error=" << error.message() << '\n';
    }} cleanup{root};
    const fs::path source=SB_BOOTSTRAP_SEED_PACK_ROOT, copy=root/"initial-resource-pack";
    fs::create_directory(copy);
    r::ResourceSeedLoadConfig load; load.seed_pack_root=source.string();
    const auto loaded=r::LoadResourceSeedPack(load); Require(loaded.ok(),"fixture resource load failed");
    // Copy only admitted data artifacts, never donor source trees.
    for (const auto& artifact:loaded.image.artifacts) {
      const fs::path relative=artifact.canonical_path;
      Require(!relative.is_absolute() && relative.string().find("..") == std::string::npos,
              "fixture artifact path escapes pack");
      fs::create_directories((copy/relative).parent_path());
      fs::copy_file(source/relative,copy/relative,fs::copy_options::skip_existing);
    }
    for (const auto* name:{"RESOURCE_SEED_MANIFEST.csv","RESOURCE_SEED_ARTIFACTS.csv"})
      fs::copy_file(source/name,copy/name);
    const auto program=Quote(fs::canonical(argv[0]).string());
    Require(std::system((program+" --create "+Quote(root.string())).c_str())==0,"create subprocess failed");
    fs::remove_all(copy);
    Require(std::system((program+" --reopen "+Quote(root.string())).c_str())==0,"independent reopen subprocess failed");
    Require(std::system((program+" --admission "+Quote(root.string())).c_str())==0,"actual resource admission subprocess failed");
    Require(std::system((program+" --behavior-read "+Quote(root.string())).c_str())==0,"actual behavior storage read subprocess failed");
    for (const auto corruption:{Corruption::artifact,Corruption::cycle,Corruption::alias_identity,Corruption::alias_epoch}) {
      const auto original=Corrupt(root/"content.sbdb",corruption);
      const auto command=program+(corruption==Corruption::cycle ? " --refuse-chain " : " --refuse-artifact ")+Quote(root.string());
      const auto refused=std::system(command.c_str());
      WritePage(root/"content.sbdb",original);
      Require(refused==0,"independent corruption refusal failed");
      std::cout << "verified_corruption_case=" << static_cast<unsigned>(corruption) << '\n';
    }
    Require(std::system((program+" --reopen "+Quote(root.string())).c_str())==0,"restored database did not reopen");
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
