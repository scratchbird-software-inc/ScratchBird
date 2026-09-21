// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "uuid.hpp"
#include "current_row_map.hpp"
#include "datatype_catalog_manifest.hpp"
#include "../../src/engine/internal_api/api_types.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_relation_metadata_store.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_heap_memory.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_binary_identity_codec.hpp"
#include "../../src/engine/internal_api/mga_relation_store/mga_binary_fields.hpp"
#include "../../src/wire/parser_server_ipc/public_resolution_cache_key.hpp"
#include "../../src/wire/parser_server_ipc/binary_identity_io.hpp"
#include "../../src/engine/executor/canonical_aggregate_registry.hpp"
#include "../../src/engine/executor/runtime_identity.hpp"
#include "../../src/engine/internal_api/query/expression_api.hpp"
#include "../../src/engine/sblr/canonical_query_sort_registration.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace uuid = scratchbird::core::uuid;
using Uuid = scratchbird::core::platform::Uuid;
using Clock = std::chrono::steady_clock;

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

static_assert(sizeof(Uuid) == 16);
static_assert(std::is_standard_layout_v<Uuid> && std::is_trivially_copyable_v<Uuid>);
using EngineUuid = scratchbird::engine::internal_api::EngineUuid;
static_assert(std::is_same_v<EngineUuid, Uuid>);
static_assert(sizeof(EngineUuid) == 16);
static_assert(std::is_standard_layout_v<EngineUuid> &&
              std::is_trivially_copyable_v<EngineUuid>);
static_assert(!std::is_constructible_v<EngineUuid, std::string>);
static_assert(!std::is_convertible_v<EngineUuid, std::string>);
static_assert(!std::is_assignable_v<EngineUuid&, std::string>);

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void RelationMetadataBinaryIdentityContract() {
  namespace api = scratchbird::engine::internal_api;
  using Statement = api::PreparedMgaHeapStatementAuthority;
  using Relation = api::PreparedMgaHeapReadAuthority;
  using Cohort = api::PreparedMgaHeapReadAuthorityCohort;
  static_assert(std::is_same_v<decltype(Statement::database_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::statement_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::transaction_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::statement_snapshot_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::statement_metadata_snapshot_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::catalog_epoch_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Statement::authorization_authority_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Relation::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(Relation::temporary_session_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaMetadataCacheKey::database_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaRelationColumnStorageDescriptor::charset_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaRelationColumnStorageDescriptor::collation_uuid), Uuid>);
  static_assert(std::is_same_v<api::DescriptorFieldsByRelation::key_type, Uuid>);
  static_assert(std::is_same_v<decltype(Cohort::relations)::key_type, Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationReadRequest::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationCountRequest::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationStreamRequest::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationReadRequest::borrowed_relation_uuid), const Uuid*>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationCountRequest::borrowed_relation_uuid), const Uuid*>);
  static_assert(std::is_same_v<decltype(api::MgaVisibleHeapRelationStreamRequest::borrowed_relation_uuid), const Uuid*>);
  using Load = api::MgaRelationStorageDescriptorLoadResult(*)(const api::EngineRequestContext&, const Uuid&);
  static_assert(std::is_same_v<decltype(&api::LoadMgaRelationStorageDescriptor), Load>);
  const Uuid base{{0,0,0,0,0,1,0x70,0,0x80,0,0,0,0,0,0,1}};
  api::DescriptorFieldsByRelation descriptors;
  Cohort cohort;
  std::map<api::MgaMetadataCacheKey, unsigned> metadata;
  unsigned count = 0;
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((byte == 6 && bit >= 4) || (byte == 8 && bit >= 6)) continue;
      auto id = base;
      id.bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
      Require(uuid::IsEngineIdentityUuid(id), "test metadata identity is not v7");
      const auto ordinal = ++count;
      Require(descriptors.emplace(id, std::vector<std::pair<std::string, std::string>>{{"marker",std::to_string(ordinal)}}).second,
              "descriptor binary keys collapsed distinct UUID bits");
      auto relation = std::make_shared<Relation>();
      relation->relation_uuid = id;
      relation->current_relation_base_generation = ordinal;
      Require(cohort.relations.emplace(id, relation).second,
              "prepared relation keys collapsed distinct UUID bits");
      api::MgaMetadataCacheKey key;
      key.database_uuid = id;
      Require(metadata.emplace(key, ordinal).second,
              "metadata key omitted database UUID bits");
      Require(descriptors.at(id).front().second == std::to_string(ordinal) &&
              cohort.relations.at(id)->relation_uuid == id && metadata.at(key) == ordinal,
              "metadata lookup changed binary identity or payload");
    }
  }
  Require(count == 122 && descriptors.size() == count && cohort.relations.size() == count && metadata.size() == count,
          "metadata identity key coverage incomplete");
  std::cout << "relation metadata binary identities=" << count << '\n';
}

void HeapBinaryIdentityCodecContract() {
  namespace api = scratchbird::engine::internal_api;
  const Uuid identity{{0,0x09,0x0a,0x7f,0x80,0xff,0x7f,0,0xbf,0,1,2,3,4,5,6}};
  const Uuid sentinel{{1,2,3,4,5,6,0x70,0,0x80,0,0,0,0,0,0,9}};
  unsigned checks = 0;
  const auto check = [&](bool pass, const char* detail) { ++checks; Require(pass, detail); };
  for (std::size_t prefix = 0; prefix < 64; ++prefix) {
    std::string encoded(prefix, '\0');
    check(api::AppendBinaryEngineUuid(&encoded, identity) && encoded.size() == prefix + 16,
          "heap identity append did not write exactly sixteen bytes");
    for (unsigned n = 0; n < 16; ++n)
      check(static_cast<unsigned char>(encoded[prefix + n]) == identity.bytes[n],
            "heap identity encoding changed network-order bytes");
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
    std::size_t offset = prefix;
    auto decoded = sentinel;
    check(api::ReadBinaryEngineUuid(bytes, &offset, &decoded) && decoded == identity && offset == prefix + 16,
          "heap identity unaligned decode changed bytes or cursor");
    for (unsigned length = 0; length < 16; ++length) {
      offset = prefix; decoded = sentinel;
      check(!api::ReadBinaryEngineUuid(bytes.first(prefix + length), &offset, &decoded) &&
            offset == prefix && decoded == sentinel, "truncated heap identity changed cursor or destination");
    }
  }
  for (unsigned version = 0; version < 16; ++version) {
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto changed = identity;
      changed.bytes[6] = static_cast<std::uint8_t>((version << 4) | 0xf);
      changed.bytes[8] = static_cast<std::uint8_t>((variant << 6) | 0x3f);
      const bool accepted = version == 7 && variant == 2;
      std::string encoded("prefix\0", 7);
      const auto original = encoded;
      check(api::AppendBinaryEngineUuid(&encoded, changed) == accepted &&
            (accepted ? encoded.size() == original.size() + 16 : encoded == original),
            "heap identity encoder admitted wrong system UUID shape or changed rejected output");
      std::size_t offset = 0;
      auto decoded = sentinel;
      check(api::ReadBinaryEngineUuid(changed.bytes, &offset, &decoded) == accepted &&
            offset == (accepted ? 16u : 0u) && decoded == (accepted ? changed : sentinel),
            "heap identity decoder admitted wrong system UUID shape or changed rejected output");
    }
  }
  std::string encoded = "unchanged";
  check(!api::AppendBinaryEngineUuid(&encoded, Uuid{}) && encoded == "unchanged" &&
        !api::AppendBinaryEngineUuid(nullptr, identity), "nil or null heap identity append accepted");
  for (const auto position : {std::size_t{0}, std::size_t{1}, std::size_t{16},
                             std::numeric_limits<std::size_t>::max() - 15,
                             std::numeric_limits<std::size_t>::max()}) {
    auto decoded = sentinel;
    auto offset = position;
    const Uuid nil{};
    check(!api::ReadBinaryEngineUuid(nil.bytes, &offset, &decoded) && offset == position && decoded == sentinel,
          "nil or wrapped heap identity cursor accepted");
  }
  std::size_t offset = 0;
  auto decoded = sentinel;
  check(!api::ReadBinaryEngineUuid(identity.bytes, nullptr, &decoded) && decoded == sentinel &&
        !api::ReadBinaryEngineUuid(identity.bytes, &offset, nullptr) && offset == 0,
        "heap identity null destination changed state");
  const std::string text = "019d0000-0000-7000-8000-00000000d701";
  check(!api::ReadBinaryEngineUuid({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, &offset, &decoded) &&
        offset == 0 && decoded == sentinel, "heap identity codec reintroduced text UUID authority");
  std::cout << "heap binary identity codec checks=" << checks << '\n';
}

void HeapBinaryFieldsContract() {
  namespace api = scratchbird::engine::internal_api;
  unsigned checks = 0;
  const auto check = [&](bool pass, const char* detail) { ++checks; Require(pass, detail); };
  const auto scalars = [&]<class UInt>(auto read) {
    for (const UInt value : {UInt{0}, UInt{1}, static_cast<UInt>(0xa5), std::numeric_limits<UInt>::max()}) {
      for (std::size_t prefix = 0; prefix < 17; ++prefix) {
        std::vector<std::uint8_t> bytes(prefix, 0xee);
        auto digits = static_cast<std::uint64_t>(value);
        for (unsigned n = 0; n < sizeof(UInt); ++n) { bytes.push_back(digits % 256); digits /= 256; }
        auto offset = prefix;
        UInt out = 17;
        check(read(bytes, &offset, &out) && offset == bytes.size() && out == value,
              "heap little-endian field oracle mismatch");
        for (std::size_t end = prefix; end < bytes.size(); ++end) {
          offset = prefix; out = 17;
          check(!read(std::span(bytes).first(end), &offset, &out) && offset == prefix && out == 17,
                "truncated scalar changed output or cursor");
        }
        for (const auto invalid : {bytes.size() + 1, std::numeric_limits<std::size_t>::max() - sizeof(UInt) + 1,
                                   std::numeric_limits<std::size_t>::max()}) {
          offset = invalid; out = 17;
          check(!read(bytes, &offset, &out) && offset == invalid && out == 17,
                "wrapped scalar cursor was dereferenced or published");
        }
        offset = prefix; out = 17;
        check(!read(bytes, nullptr, &out) && out == 17 && !read(bytes, &offset, nullptr) && offset == prefix,
              "null scalar output changed state");
      }
    }
  };
  scalars.template operator()<std::uint8_t>(api::ReadBinaryU8);
  scalars.template operator()<std::uint16_t>(api::ReadBinaryU16);
  scalars.template operator()<std::uint32_t>(api::ReadBinaryU32);
  scalars.template operator()<std::uint64_t>(api::ReadBinaryU64);
  for (const unsigned length : {0u,1u,16u,255u,1024u,4096u}) {
    for (const unsigned prefix : {0u,1u,7u,31u}) {
      std::string expected(length, '\0');
      for (unsigned n = 0; n < length; ++n) expected[n] = static_cast<char>((n * 73) % 256);
      std::vector<std::uint8_t> bytes(prefix, 0xfa);
      auto word = length;
      for (unsigned n = 0; n < 4; ++n) { bytes.push_back(word % 256); word /= 256; }
      bytes.insert(bytes.end(), expected.begin(), expected.end());
      std::size_t offset = prefix;
      std::string out = "sentinel";
      check(api::ReadBinaryString(bytes, &offset, &out) && offset == bytes.size() && out == expected,
            "heap framed byte-string oracle mismatch");
      for (std::size_t end = prefix; end < bytes.size(); ++end) {
        offset = prefix; out = "sentinel";
        check(!api::ReadBinaryString(std::span(bytes).first(end), &offset, &out) && offset == prefix && out == "sentinel",
              "truncated string consumed its length or changed caller output");
      }
      std::fill_n(bytes.begin() + prefix, 4, 0xff);
      offset = prefix; out = "sentinel";
      check(!api::ReadBinaryString(bytes, &offset, &out) && offset == prefix && out == "sentinel",
            "oversized framed string published output");
      offset = std::numeric_limits<std::size_t>::max();
      check(!api::ReadBinaryString(bytes, &offset, &out) && offset == std::numeric_limits<std::size_t>::max() && out == "sentinel",
            "wrapped string cursor changed caller state");
      offset = 0;
      check(!api::ReadBinaryString(bytes, nullptr, &out) && out == "sentinel" &&
            !api::ReadBinaryString(bytes, &offset, nullptr) && offset == 0,
            "null framed string output changed caller state");
    }
  }
  std::cout << "heap binary field decoder checks=" << checks << '\n';
}

void HeapBinaryMemoryContract() {
  namespace api = scratchbird::engine::internal_api;
  const auto string_bytes = [](const std::string& value) {
    return static_cast<std::uint64_t>(value.capacity()) + 1;
  };
  unsigned checks = 0;
  const auto check = [&](bool pass, const char* detail) { ++checks; Require(pass, detail); };
  const auto max = std::numeric_limits<std::uint64_t>::max();
  for(bool binary:{false,true}) {
    api::EngineEvidenceReference evidence{"fixture.evidence.kind",std::string(80,'v')};
    if(binary)evidence.evidence_id=Uuid{};
    const auto expected=string_bytes(evidence.evidence_kind)+
        (binary ? 0 : string_bytes(std::get<std::string>(evidence.evidence_id)));
    std::uint64_t total=31;
    check(api::AccountHeapReadEvidenceDynamicMemory(evidence,&total) && total==31+expected,
          "evidence memory accounting charged binary reference as text or lost text capacity");
    total=max-expected;
    check(api::AccountHeapReadEvidenceDynamicMemory(evidence,&total) && total==max,
          "evidence accounting lost exact upper boundary");
    total=max-expected+1;const auto prior=total;
    check(!api::AccountHeapReadEvidenceDynamicMemory(evidence,&total) && total==prior,
          "evidence accounting overflow changed output");
    check(!api::AccountHeapReadEvidenceDynamicMemory(evidence,nullptr),"evidence accounting accepted null output");
  }
  for (const auto value : {std::uint64_t{0}, std::uint64_t{1}, max / 2, max}) {
    std::uint64_t total = max - value;
    check(api::CheckedHeapReadMemoryAdd(value, &total) && total == max, "heap byte sum lost boundary value");
    total = max;
    check(api::CheckedHeapReadMemoryAdd(value, &total) == (value == 0) && total == max,
          "heap byte overflow changed output or succeeded");
    std::uint64_t product = 71;
    check(api::CheckedHeapReadMemoryMultiply(value, 1, &product) && product == value,
          "heap byte product changed exact value");
    product = 71;
    check(api::CheckedHeapReadMemoryMultiply(value, 2, &product) == (value <= max / 2) &&
          product == (value <= max / 2 ? value * 2 : 71), "heap multiplication overflow changed output");
  }
  check(!api::CheckedHeapReadMemoryAdd(0, nullptr) &&
        !api::CheckedHeapReadMemoryMultiply(0, 0, nullptr), "heap arithmetic accepted null output");
  using Row = api::CrudRowVersionRecord;
  for (unsigned count = 0; count <= 64; ++count) {
    std::vector<Row> rows(count);
    rows.reserve(count + 3);
    std::uint64_t expected = sizeof(rows) + rows.capacity() * sizeof(Row);
    for (unsigned n = 0; n < count; ++n) {
      auto& row = rows[n];
      row.values.reserve(3);
      row.values.emplace_back("payload", std::string(n + 17, 'x'));
      const auto dynamic = row.values.capacity() * sizeof(row.values[0]) +
          string_bytes(row.values[0].first) + string_bytes(row.values[0].second);
      expected += dynamic;
      std::uint64_t measured = 13;
      check(api::AccountHeapReadRowDynamicMemoryBytes(row, &measured) && measured == 13 + dynamic,
            "row dynamic accounting charged inline UUIDs or omitted value capacity");
    }
    check(api::HeapReadRowVectorMemoryBytes(rows) == expected, "row vector accounting differs from independent size oracle");
    for (auto& row : rows) {
      row.table_uuid.bytes.fill(0xa5); row.row_uuid.bytes.fill(0x5a);
      row.version_uuid.bytes.fill(0x01); row.previous_version_uuid.bytes.fill(0xfe);
      row.temporary_session_uuid.bytes.fill(0x7f);
    }
    check(api::HeapReadRowVectorMemoryBytes(rows) == expected, "fixed UUID contents changed row memory charge");
    using Versions = std::unordered_map<Uuid, const Row*, api::EngineUuidHash>;
    using Visible = std::unordered_map<Uuid, std::size_t, api::EngineUuidHash>;
    check(api::HeapReadVersionIndexProjectionMemoryBytes(rows) ==
          sizeof(Versions) + count * (2 * sizeof(void*) + sizeof(Versions::value_type) + 4 * sizeof(void*)),
          "version projection still accounts text keys");
    check(api::HeapReadVisibilityMapProjectionMemoryBytes(rows) ==
          sizeof(Visible) + count * (2 * sizeof(void*) + sizeof(Visible::value_type) + 4 * sizeof(void*)),
          "visibility projection still accounts text keys");
    Visible visible;
    for (unsigned n = 0; n < count; ++n) { Uuid id{}; id.bytes[15] = static_cast<std::uint8_t>(n); visible.emplace(id, n); }
    check(api::HeapReadVisibilityMapMemoryBytes(visible) ==
          sizeof(visible) + visible.bucket_count() * sizeof(void*) + count * (sizeof(Visible::value_type) + 4 * sizeof(void*)),
          "visibility map accounting differs from binary node oracle");
  }
  api::MgaRelationStorageDescriptor descriptor;
  const auto fixed = api::HeapReadStorageDescriptorMemoryBytes(descriptor);
  check(fixed.has_value() && *fixed >= sizeof(descriptor), "descriptor accounting omitted fixed fields");
  descriptor.descriptor_uuid.bytes.fill(1); descriptor.database_uuid.bytes.fill(2);
  descriptor.schema_uuid.bytes.fill(3); descriptor.relation_uuid.bytes.fill(4);
  descriptor.primary_filespace_uuid.bytes.fill(5);
  check(api::HeapReadStorageDescriptorMemoryBytes(descriptor) == fixed, "descriptor UUID contents changed memory charge");
  descriptor.columns.resize(1);
  const auto with_column = api::HeapReadStorageDescriptorMemoryBytes(descriptor);
  auto& column = descriptor.columns.front();
  column.column_uuid.bytes.fill(6); column.charset_uuid.bytes.fill(7); column.collation_uuid.bytes.fill(8);
  column.value_descriptor.descriptor_uuid.bytes.fill(9); column.value_descriptor.type_uuid.bytes.fill(10);
  column.value_descriptor.collation_uuid.bytes.fill(11);
  check(with_column.has_value() && api::HeapReadStorageDescriptorMemoryBytes(descriptor) == with_column,
        "column resource UUID contents changed memory charge");
  const auto before = string_bytes(column.canonical_name_key);
  column.canonical_name_key.assign(4096, 'c');
  check(api::HeapReadStorageDescriptorMemoryBytes(descriptor) ==
        *with_column - before + string_bytes(column.canonical_name_key), "descriptor name growth escaped memory accounting");
  std::cout << "heap binary memory checks=" << checks << '\n';
}

void CurrentRowBinaryIdentityContract() {
  namespace mga = scratchbird::transaction::mga;
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapEntry::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapEntry::row_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapEntry::current_version_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapObservedFacts::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapObservedFacts::row_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowAuthoritativeBaseRow::row_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowAuthoritativeBaseRow::version_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapRebuildRequest::relation_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapDecision::row_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(mga::CurrentRowMapDecision::current_version_uuid), Uuid>);
  const auto id = [](std::uint64_t value) {
    Uuid out{{1,2,3,4,5,6,0x70,0,0x80,0,0,0,0,0,0,0}};
    for (unsigned i=0; i<7; ++i) out.bytes[15-i]=static_cast<unsigned char>(value>>(8*i));
    return out;
  };
  unsigned checks=0;
  const auto check=[&](bool ok,const char* detail){++checks;Require(ok,detail);};
  mga::CurrentRowMapRebuildRequest request;
  request.relation_uuid=id(1);
  request.relation_epoch=2;request.catalog_epoch=3;request.security_epoch=4;
  request.redaction_epoch=5;request.map_generation=6;request.invalidation_generation=7;
  request.authoritative_base_rows_proof=true;request.durable_mga_inventory_proof=true;
  for (unsigned i=0;i<512;++i)
    request.base_rows.push_back({id(2+i*2),id(3+i*2),8,{9},false,true});
  auto rebuilt=mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(request);
  check(rebuilt.ok && rebuilt.rebuilt_entry_count==512 && rebuilt.map.entries.size()==512,
        "binary current-row map did not rebuild actual input entries");
  mga::CurrentRowMapObservedFacts facts;
  facts.relation_uuid=request.relation_uuid;
  facts.relation_epoch=2;facts.catalog_epoch=3;facts.security_epoch=4;
  facts.redaction_epoch=5;facts.map_generation=6;facts.invalidation_generation=7;
  facts.reader_visible_through_local_transaction_id={10};
  facts.oldest_active_local_transaction_id={11};
  facts.durable_mga_inventory_proof=true;facts.transaction_horizon_authoritative=true;
  facts.normal_mga_visibility_authority_available=true;facts.security_recheck_planned=true;
  for (const auto& row:request.base_rows) {
    facts.row_uuid=row.row_uuid;
    const auto before=rebuilt.map.counters;
    const auto d=mga::LookupCurrentRowMap(&rebuilt.map,facts);
    check(d.accepted && d.row_uuid==row.row_uuid && d.current_version_uuid==row.version_uuid &&
          d.normal_mga_recheck_required && d.security_recheck_required &&
          !d.map_is_visibility_authority && !d.map_is_transaction_finality_authority,
          "binary current-row candidate changed identities or became authority");
    check(rebuilt.map.counters.probes==before.probes+1 &&
          rebuilt.map.counters.accepted==before.accepted+1 &&
          rebuilt.map.counters.refused==before.refused,
          "current-row lookup double-counted probe or accepted outcome");
    check(std::none_of(d.evidence.begin(),d.evidence.end(),[](const auto& field) {
            return field.name=="row_uuid" || field.name=="current_version_uuid";
          }),"current-row identity escaped through string evidence");
  }
  request.base_rows.resize(1);facts.row_uuid=request.base_rows.front().row_uuid;
  const auto refuse_rebuild=[&](const mga::CurrentRowMapRebuildRequest& bad,const char* reason) {
    const auto result=mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(bad);
    check(!result.ok && result.diagnostic_code=="CATALOG.INVALID_INPUT" &&
          result.refusal_reason==reason && result.map.entries.empty() &&
          result.rebuilt_entry_count==0 && result.counters.refused==1,
          "invalid current-row rebuild published a partial map or wrong outcome");
  };
  const auto refuse_entry=[&](const mga::CurrentRowMapEntry& entry,
                              const mga::CurrentRowMapObservedFacts& observed) {
    const auto before=rebuilt.map.counters;
    const auto d=mga::EvaluateCurrentRowMapEntry(&rebuilt.map,entry,observed);
    check(!d.accepted && d.row_uuid.is_nil() && d.current_version_uuid.is_nil() &&
          d.normal_mga_recheck_required && d.security_recheck_required,
          "invalid current-row candidate published identity evidence");
    check(rebuilt.map.counters.probes==before.probes+1 &&
          rebuilt.map.counters.refused==before.refused+1 &&
          rebuilt.map.counters.accepted==before.accepted,
          "direct current-row refusal was not counted exactly once");
  };
  const auto entry=rebuilt.map.entries.front();
  for (unsigned field=0;field<5;++field) {
    for (unsigned version=0;version<16;++version) {
      if (version==7) continue;
      auto bad_entry=entry;auto bad_facts=facts;
      Uuid* slots[]={&bad_entry.relation_uuid,&bad_entry.row_uuid,&bad_entry.current_version_uuid,
                     &bad_facts.relation_uuid,&bad_facts.row_uuid};
      slots[field]->bytes[6]=static_cast<unsigned char>(version<<4);
      refuse_entry(bad_entry,bad_facts);
    }
    auto bad_entry=entry;auto bad_facts=facts;
    Uuid* slots[]={&bad_entry.relation_uuid,&bad_entry.row_uuid,&bad_entry.current_version_uuid,
                   &bad_facts.relation_uuid,&bad_facts.row_uuid};
    *slots[field]={};refuse_entry(bad_entry,bad_facts);
    *slots[field]=id(2);slots[field]->bytes[8]=0xc0;refuse_entry(bad_entry,bad_facts);
  }
  for(unsigned field=0;field<3;++field) for(unsigned version=0;version<16;++version) {
    if(version==7)continue;
    auto bad=request;
    Uuid* slots[]={&bad.relation_uuid,&bad.base_rows[0].row_uuid,&bad.base_rows[0].version_uuid};
    slots[field]->bytes[6]=static_cast<unsigned char>(version<<4);
    refuse_rebuild(bad,field==0 ? "current_row_rebuild_identity_or_epoch_missing" :
                                "current_row_rebuild_base_row_invalid");
  }
  auto bad=request;bad.base_rows.push_back(request.base_rows.front());
  refuse_rebuild(bad,"current_row_rebuild_duplicate_identity");
  bad.base_rows.back().row_uuid=id(9000);
  refuse_rebuild(bad,"current_row_rebuild_duplicate_identity");
  bad=request;bad.base_rows.push_back(request.base_rows.front());
  bad.base_rows.back().version_uuid={};
  refuse_rebuild(bad,"current_row_rebuild_base_row_invalid");
  bad.base_rows.back().deleted=true;
  refuse_rebuild(bad,"current_row_rebuild_base_row_invalid");
  bad.base_rows.back().deleted=false;bad.base_rows.back().visible=false;
  refuse_rebuild(bad,"current_row_rebuild_base_row_invalid");
  bad=request;bad.base_rows.front().row_generation=0;
  refuse_rebuild(bad,"current_row_rebuild_base_row_invalid");
  bad=request;bad.base_rows.front().visible_through_local_transaction_id={};
  refuse_rebuild(bad,"current_row_rebuild_base_row_invalid");
  bad=request;bad.base_rows.front().deleted=true;
  auto empty=mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(bad);
  check(empty.ok && empty.map.entries.empty(),"valid deleted row was not omitted");
  bad.base_rows.front().deleted=false;bad.base_rows.front().visible=false;
  empty=mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(bad);
  check(empty.ok && empty.map.entries.empty(),"valid invisible row was not omitted");
  auto duplicate=mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(request).map;
  duplicate.entries.push_back(duplicate.entries.front());
  const auto ambiguous=mga::LookupCurrentRowMap(&duplicate,facts);
  check(!ambiguous.accepted && ambiguous.refusal_reason=="current_row_map_duplicate_identity" &&
        ambiguous.row_uuid.is_nil() && ambiguous.current_version_uuid.is_nil() &&
        duplicate.counters.probes==1 && duplicate.counters.refused==1 && duplicate.counters.accepted==0,
        "duplicate current-row key selected first match or double-counted failure");
  auto stale=facts;stale.map_generation++;
  const auto before=rebuilt.map.counters;
  const auto d=mga::LookupCurrentRowMap(&rebuilt.map,stale);
  check(!d.accepted && rebuilt.map.counters.probes==before.probes+1 &&
        rebuilt.map.counters.refused==before.refused+1,
        "stale-map wrapper double-counted refusal");
  std::cout << "PASS binary current-row advisory identity checks=" << checks << '\n';
}

void TypedDescriptorBinaryIdentityContract() {
  namespace api = scratchbird::engine::internal_api;
  static_assert(std::is_same_v<decltype(api::EngineDescriptor::type_uuid), Uuid>);
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid{{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,16}};
  descriptor.type_uuid = Uuid{{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,17}};
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = "nullability=non_null";
  Require(api::QowCanonicalDescriptorIdentityV1(descriptor),
          "typed descriptor rejected present binary identity fields");
  Require(descriptor == descriptor, "exact descriptor equality is not reflexive");
  for (const auto field : {"descriptor_uuid", "datatype_descriptor_uuid", "type_uuid", "collation_uuid"}) {
    auto dual = descriptor;
    dual.encoded_descriptor += ";" + std::string(field) + "=01020304-0506-7708-890a-0b0c0d0e0f11";
    Require(!api::QowCanonicalDescriptorIdentityV1(dual),
            "textual duplicate identity survived alongside binary descriptor authority");
  }
  for (auto member : {&api::EngineDescriptor::descriptor_uuid,
                      &api::EngineDescriptor::type_uuid}) {
    auto invalid = descriptor;
    invalid.*member = {};
    Require(invalid != descriptor, "exact descriptor equality omitted a binary identity");
    // A text spelling cannot supply missing binary authority.
    invalid.encoded_descriptor += ";type_uuid=01020304-0506-7708-890a-0b0c0d0e0f11";
    Require(!api::QowCanonicalDescriptorIdentityV1(invalid),
            "descriptor text substituted for missing binary identity");
    for (unsigned version = 0; version < 16; ++version) {
      invalid = descriptor;
      (invalid.*member).bytes[6] = static_cast<std::uint8_t>((version << 4) | 7);
      Require(api::QowCanonicalDescriptorIdentityV1(invalid) == (version == 7),
              "typed descriptor system identity version policy drifted");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
      invalid = descriptor;
      (invalid.*member).bytes[8] = static_cast<std::uint8_t>((variant << 6) | 9);
      Require(api::QowCanonicalDescriptorIdentityV1(invalid) == (variant == 2),
              "typed descriptor system identity variant policy drifted");
    }
  }
  std::cout << "PASS typed descriptor binary identity admission; not catalog authority lookup\n";
}

void AggregateRegistryBinaryIdentityContract() {
  namespace exec = scratchbird::engine::executor;
  static_assert(std::is_same_v<decltype(exec::CanonicalAggregateRegistryEntry::function_uuid), Uuid>);
  // Independent authoritative identities from builtin-aggregate-registry.yaml.
  // Coverage of this private registry is not aggregate execution conformance.
  const std::array<std::pair<std::string_view, Uuid>, 49> oracle{{
      {"sb.aggregate.count", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x78,0x4a,0x9a,0xec,0x37,0x1f,0x8b,0x95,0xb7,0xea}}},
      {"sb.aggregate.sum", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x72,0xe4,0x85,0x49,0x82,0xb2,0xee,0xf5,0xa7,0x77}}},
      {"sb.aggregate.avg", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x78,0xac,0xb5,0x0c,0x45,0xb8,0x32,0x83,0x10,0x04}}},
      {"sb.aggregate.min", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x78,0x1c,0x88,0x1b,0x4a,0xf4,0xd5,0x5d,0x40,0x2b}}},
      {"sb.aggregate.max", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x7d,0x1e,0x8a,0xa4,0x80,0xbc,0x64,0x7f,0xbd,0x9a}}},
      {"sb.aggregate.bool_and", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x78,0xb0,0xad,0x98,0xa6,0x81,0xe9,0x3b,0x4c,0x49}}},
      {"sb.aggregate.bool_or", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x7c,0x2a,0xa3,0xf2,0xe4,0xb9,0xd3,0x6d,0xf4,0x03}}},
      {"sb.aggregate.array_agg", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x71,0x59,0x9f,0x7b,0x91,0x55,0x13,0xb8,0xc0,0xd4}}},
      {"sb.aggregate.string_agg", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x72,0x43,0xab,0xc6,0x4f,0x6a,0x77,0x7d,0xff,0x00}}},
      {"sb.aggregate.json_agg", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x01,0x70,0x21,0x8a,0x00,0x00,0x00,0x00,0x00,0x00,0x23}}},
      {"sb.aggregate.json_object_agg", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x01,0x70,0x21,0x8a,0x00,0x00,0x00,0x00,0x00,0x00,0x24}}},
      {"sb.aggregate.stddev_pop", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x73,0xc9,0xba,0x10,0x46,0x65,0xf7,0x41,0x21,0x5d}}},
      {"sb.aggregate.variance_pop", Uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x7f,0xda,0xb4,0x70,0xe8,0x54,0x14,0xdc,0xb3,0x14}}},
      {"sb.aggregate.every", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x78,0x76,0x96,0x44,0xae,0x83,0xb3,0x63,0xd3,0xbc}}},
      {"sb.aggregate.listagg", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7e,0x93,0x8e,0x4d,0x60,0x63,0x84,0x9d,0xe0,0x49}}},
      {"sb.aggregate.rank", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x73,0x36,0xab,0x53,0xfe,0xf5,0x31,0x62,0x20,0xd7}}},
      {"sb.aggregate.dense_rank", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7b,0xd3,0xa7,0x31,0x17,0x34,0x58,0x1e,0xb8,0xce}}},
      {"sb.aggregate.percent_rank", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x78,0x17,0x91,0x1f,0x9f,0x8b,0x2e,0x66,0xeb,0xec}}},
      {"sb.aggregate.cume_dist", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x72,0x44,0x89,0xfd,0x8f,0xa6,0x6a,0xe9,0x30,0xd5}}},
      {"sb.aggregate.mode", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x71,0x50,0x9b,0xe6,0xbc,0xf9,0x7f,0x8f,0xac,0xf5}}},
      {"sb.aggregate.percentile_cont", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7c,0xfd,0x83,0xdd,0x15,0x43,0x5f,0xe5,0x5b,0xf5}}},
      {"sb.aggregate.percentile_disc", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x70,0x81,0xb7,0x66,0x7d,0xb8,0x18,0xa8,0x9c,0x04}}},
      {"sb.aggregate.approx_count_distinct", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x77,0x36,0x96,0xf3,0xe2,0x0c,0xbd,0x53,0x2b,0xa5}}},
      {"sb.aggregate.approx_median", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7c,0xe0,0x85,0xa6,0xcb,0xcd,0x71,0xf2,0xc8,0x6e}}},
      {"sb.aggregate.approx_percentile_cont", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x76,0xdf,0x98,0xa6,0xaa,0x77,0xd1,0xa3,0x42,0xf8}}},
      {"sb.aggregate.approx_percentile_disc", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x75,0x78,0xa8,0x8f,0x8d,0xb4,0xbb,0x64,0x97,0x55}}},
      {"sb.aggregate.approx_top_k", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7f,0x47,0x8f,0xe1,0x0c,0x5e,0x0e,0xc8,0x7b,0xf0}}},
      {"sb.aggregate.stddev", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x74,0x75,0x85,0x16,0xff,0x00,0x3b,0x2b,0xda,0xd9}}},
      {"sb.aggregate.variance", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x79,0x68,0x82,0xc5,0x04,0xcf,0xfb,0xeb,0x97,0x1b}}},
      {"sb.aggregate.stddev_samp", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7d,0x99,0xa4,0x95,0x70,0xf9,0xc3,0xb1,0xb5,0x87}}},
      {"sb.aggregate.variance_samp", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x73,0x2b,0x8a,0x0c,0x2a,0xa8,0x8b,0x04,0xf3,0xc5}}},
      {"sb.aggregate.corr", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x77,0xbb,0xba,0x9b,0x2e,0x78,0xac,0xf8,0x45,0x21}}},
      {"sb.aggregate.covar_pop", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7f,0x09,0x8c,0xeb,0x17,0xad,0x4c,0x70,0xe9,0x9f}}},
      {"sb.aggregate.covar_samp", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x74,0x7d,0xbc,0x01,0xca,0xad,0x91,0x37,0xd0,0x70}}},
      {"sb.aggregate.regr_count", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x75,0xaa,0xbb,0xe6,0xa4,0xa6,0x7d,0xac,0xb8,0x1f}}},
      {"sb.aggregate.regr_avgx", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x76,0x62,0xa8,0x16,0xd1,0xdf,0x50,0xe9,0xb6,0x64}}},
      {"sb.aggregate.regr_avgy", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7d,0x03,0xac,0x2d,0x75,0x3c,0xdb,0x77,0x44,0xc0}}},
      {"sb.aggregate.regr_intercept", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7c,0x7c,0xb5,0x76,0xd6,0x7e,0xa9,0xd4,0xbc,0xbb}}},
      {"sb.aggregate.regr_r2", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7a,0x43,0x9a,0x28,0xa1,0x19,0xb3,0x1d,0x9c,0x20}}},
      {"sb.aggregate.regr_slope", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7f,0x80,0xb8,0x1a,0x52,0x40,0xa6,0xdb,0xab,0x55}}},
      {"sb.aggregate.regr_sxx", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x73,0x5e,0x9e,0x55,0x5f,0x92,0x43,0x78,0x64,0x03}}},
      {"sb.aggregate.regr_sxy", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x78,0x8b,0xa2,0x49,0x86,0x65,0x47,0xa4,0x3e,0xbe}}},
      {"sb.aggregate.regr_syy", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x74,0xf7,0x98,0xba,0xc2,0x4e,0xad,0x6d,0x30,0xdf}}},
      {"timeseries.aggregate", Uuid{{0x01,0x9f,0x00,0x00,0x00,0x00,0x70,0x00,0x80,0x00,0x00,0x00,0x00,0x06,0x39,0x01}}},
      {"sb.json.jsonb_agg", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x76,0x9f,0x9e,0xac,0xf2,0x62,0x24,0x14,0xc1,0x3f}}},
      {"sb.aggregate.any_value", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7b,0x3c,0xa6,0xa7,0xfa,0x67,0x78,0xf3,0xe1,0x07}}},
      {"sb.aggregate.any_value_expr", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x73,0x9f,0xb4,0xb5,0x78,0x45,0x01,0x0a,0x08,0x8c}}},
      {"sb.aggregate.collect", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x76,0x1a,0x9f,0x4d,0xeb,0xdc,0x46,0x04,0xf3,0xd7}}},
      {"sb.aggregate.collect_expr", Uuid{{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x7e,0xbf,0x84,0x2f,0x2d,0x93,0x58,0x4f,0x07,0x8f}}},
  }};
  Require(exec::ValidateCanonicalAggregateRuntimeRegistryV1().empty(),
          "aggregate identity registry validation failed");
  const auto& registry = exec::CanonicalAggregateRuntimeRegistryV1();
  Require(!registry.empty(), "aggregate runtime identity registry disappeared");
  for (const auto& row : registry) {
    const auto expected = std::ranges::find_if(oracle, [&](const auto& item) {
      return item.first == row.builtin_id;
    });
    Require(expected != oracle.end() && expected->second == row.function_uuid &&
            uuid::IsEngineIdentityUuid(row.function_uuid),
            "aggregate runtime identity differs from its Core registry row");
    Require(exec::LookupCanonicalAggregateByUuidV1(expected->second) == &row &&
            exec::LookupCanonicalAggregateByBuiltinIdV1(expected->first) == &row &&
            exec::LookupCanonicalAggregateByFunctionV1(row.function) == &row &&
            exec::LookupCanonicalAggregateExactV1(1, row.function, expected->first,
                                                   expected->second) == &row,
            "binary aggregate lookup did not return the exact stable row");
    Require(exec::LookupCanonicalAggregateExactV1(0, row.function, expected->first,
                                                   expected->second) == nullptr &&
            exec::LookupCanonicalAggregateExactV1(2, row.function, expected->first,
                                                   expected->second) == nullptr &&
            exec::LookupCanonicalAggregateExactV1(1, row.function, "wrong",
                                                   expected->second) == nullptr,
            "aggregate exact lookup discarded an ABI or builtin fence");
    for (unsigned byte = 0; byte != 16; ++byte) {
      for (unsigned bit = 0; bit != 8; ++bit) {
        auto changed = expected->second;
        changed.bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);
        const auto existing = std::ranges::find_if(registry, [&](const auto& item) {
          return item.function_uuid.bytes == changed.bytes;
        });
        const auto found = exec::LookupCanonicalAggregateByUuidV1(changed);
        Require(found == (existing == registry.end() ? nullptr : &*existing),
                "aggregate UUID lookup ignored an identity bit");
      }
    }
  }
  Require(exec::LookupCanonicalAggregateByUuidV1({}) == nullptr,
          "aggregate UUID lookup admitted nil");
  std::cout << "PASS " << registry.size()
            << " existing aggregate registry binary identities; not full execution coverage\n";
}

void BinaryIdentityDecodeContract() {
  using scratchbird::wire::parser_server_ipc::ReadEngineIdentityUuid;
  const Uuid expected{{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,16}};
  const Uuid sentinel{{9,8,7,6,5,4,0x73,2,0x81,10,11,12,13,14,15,16}};
  std::vector<std::uint8_t> data{0xaa};
  data.insert(data.end(), expected.bytes.begin(), expected.bytes.end());
  data.push_back(0xbb);
  Uuid out = sentinel;
  std::size_t offset = 1;
  Require(ReadEngineIdentityUuid(data, &offset, &out) &&
          offset == 17 && out == expected && data[offset] == 0xbb,
          "unaligned identity decode changed bytes or consumed a following field");
  for (std::size_t size = 0; size < 17; ++size) {
    offset = 1;
    out = sentinel;
    Require(!ReadEngineIdentityUuid(std::span(data).first(size), &offset, &out) &&
            offset == 1 && out == sentinel,
            "truncated identity published partial decode state");
  }
  for (const std::size_t invalid_offset : {data.size(), data.size() + 1,
                                          std::numeric_limits<std::size_t>::max()}) {
    offset = invalid_offset;
    out = sentinel;
    Require(!ReadEngineIdentityUuid(data, &offset, &out) &&
            offset == invalid_offset && out == sentinel,
            "identity bounds check overflowed or modified output");
  }
  for (unsigned version = 0; version < 16; ++version) {
    data[7] = static_cast<std::uint8_t>((version << 4) | 7);
    offset = 1;
    out = sentinel;
    const bool accepted = ReadEngineIdentityUuid(data, &offset, &out);
    Require(accepted == (version == 7), "non-v7 system identity was admitted");
    if (!accepted) Require(offset == 1 && out == sentinel,
                           "invalid version changed decoder state");
  }
  data[7] = expected.bytes[6];
  for (unsigned variant = 0; variant < 4; ++variant) {
    data[9] = static_cast<std::uint8_t>((variant << 6) | 9);
    offset = 1;
    out = sentinel;
    const bool accepted = ReadEngineIdentityUuid(data, &offset, &out);
    Require(accepted == (variant == 2), "non-RFC system identity was admitted");
    if (!accepted) Require(offset == 1 && out == sentinel,
                           "invalid variant changed decoder state");
  }
  std::fill(data.begin() + 1, data.begin() + 17, 0);
  offset = 1;
  out = sentinel;
  Require(!ReadEngineIdentityUuid(data, &offset, &out) && offset == 1 && out == sentinel,
          "required system identity admitted nil");
  Require(ReadEngineIdentityUuid(data, &offset, &out, true) &&
          offset == 17 && out.is_nil(), "optional system identity rejected nil");
  out = sentinel;
  offset = 1;
  Require(!ReadEngineIdentityUuid(data, nullptr, &out) && out == sentinel &&
          !ReadEngineIdentityUuid(data, &offset, nullptr) && offset == 1,
          "null decoder destination was not rejected atomically");
  std::cout << "PASS binary identity decoder bounds, versions, variants and atomic rejection\n";
}

void PublicResolutionCacheIdentityContract() {
  namespace ipc = scratchbird::parser::ipc;
  namespace cache = ipc::public_resolution_cache;
  ipc::ParserSessionContext session;
  const Uuid first{{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,16}};
  auto second = first;
  second.bytes[15] = 17;
  session.session_uuid = first;
  session.search_path = {"sys", "public"};
  session.effective_role_uuids = {first, second};
  session.effective_group_uuids = {second, first};
  const auto baseline = cache::SbpsClientPublicResolutionScopeKey("endpoint", session);
  // Independent prefix oracle: u64 little-endian text length, exact text,
  // then raw UUID bytes including zeroes in subsequent nil identities.
  const std::vector<std::uint8_t> prefix{
      8,0,0,0,0,0,0,0,'e','n','d','p','o','i','n','t',
      1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,16,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  Require(baseline.size() >= prefix.size() &&
          std::equal(prefix.begin(), prefix.end(), baseline.begin()),
          "cache scope changed the binary UUID/framing layout");
  for (auto member : {&ipc::ParserSessionContext::session_uuid,
                      &ipc::ParserSessionContext::connection_uuid,
                      &ipc::ParserSessionContext::database_uuid,
                      &ipc::ParserSessionContext::authenticated_user_uuid,
                      &ipc::ParserSessionContext::dialect_profile_uuid,
                      &ipc::ParserSessionContext::policy_profile_uuid}) {
    for (unsigned byte = 0; byte != 16; ++byte) {
      auto changed = session;
      (changed.*member).bytes[byte] ^= 0x80;
      Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", changed) != baseline,
              "cache scope ignored an identity byte");
    }
  }
  for (auto member : {&ipc::ParserSessionContext::catalog_epoch,
                      &ipc::ParserSessionContext::security_policy_epoch,
                      &ipc::ParserSessionContext::grant_epoch,
                      &ipc::ParserSessionContext::descriptor_epoch,
                      &ipc::ParserSessionContext::localized_name_epoch,
                      &ipc::ParserSessionContext::language_resource_epoch,
                      &ipc::ParserSessionContext::message_resource_epoch}) {
    auto changed = session;
    changed.*member = 0x0102030405060708ULL;
    Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", changed) != baseline,
            "cache scope ignored a generation fence");
  }
  auto reordered = session;
  std::reverse(reordered.effective_role_uuids.begin(), reordered.effective_role_uuids.end());
  std::reverse(reordered.effective_group_uuids.begin(), reordered.effective_group_uuids.end());
  Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", reordered) == baseline,
          "unordered authority identity sets changed cache identity");
  std::reverse(reordered.search_path.begin(), reordered.search_path.end());
  Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", reordered) != baseline,
          "cache scope erased search-path order");
  auto left = session, right = session;
  left.principal_claim = "a|auth_provider=b";
  left.auth_provider_family = "c";
  right.principal_claim = "a";
  right.auth_provider_family = "b|auth_provider=c";
  Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", left) !=
          cache::SbpsClientPublicResolutionScopeKey("endpoint", right),
          "text separator injection collided in cache scope");
  left.search_path = {"a,b", "c"};
  right = left;
  right.search_path = {"a", "b,c"};
  Require(cache::SbpsClientPublicResolutionScopeKey("endpoint", left) !=
          cache::SbpsClientPublicResolutionScopeKey("endpoint", right),
          "search-path element boundaries collided");
  ipc::ParserClientConfig config;
  const auto resolved = cache::SbpsClientResolveNameCacheKey(
      "endpoint", session, "name", false, "table", config);
  const auto rendered = cache::SbpsClientRenderUuidCacheKey("endpoint", session, first);
  Require(std::equal(baseline.begin(), baseline.end(), resolved.begin()) &&
          std::equal(baseline.begin(), baseline.end(), rendered.begin()) &&
          resolved[baseline.size()] == 1 && rendered[baseline.size()] == 2 &&
          rendered.size() == baseline.size() + 17 &&
          std::equal(first.bytes.begin(), first.bytes.end(), rendered.end() - 16),
          "cache operation scope or binary render identity changed");
  Require(resolved != cache::SbpsClientResolveNameCacheKey(
      "endpoint", session, "name", true, "table", config),
          "cache ignored quoted-name semantics");
  std::cout << "PASS binary parser cache identities, framing and ordered search-path fences\n";
}

void CatalogIdentityContract() {
  namespace dt = scratchbird::core::datatypes;
  static_assert(std::is_same_v<decltype(dt::DatatypeTypeCodecIdentityRowV1::descriptor_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(dt::DatatypeTypeCodecIdentityRowV1::catalog_snapshot_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(dt::DatatypeTypeCodecIdentityRowV1::type_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(dt::DatatypeTypeCodecIdentityRowV1::codec_uuid), Uuid>);
  static_assert(std::is_same_v<decltype(dt::BuiltinOperatorTypeCodecIdentityRowV1::operator_uuid), Uuid>);
  // Independent bytes from the manifest-listed datatype identity registry.
  // This helper is a test fixture, never runtime identity issuance or lookup.
  const auto registered = [](std::uint8_t suffix) {
    return Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,suffix}};
  };
  const Uuid boolean{{0x01,0,0,0,0x62,0x6f,0x7f,0x6c,0xa5,0x61,0x6e,0,0,0,0,0}};
  const Uuid decimal{{0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}};
  const Uuid snapshot = registered(0x01);
  const std::array<std::pair<Uuid,Uuid>,6> expected{{
      {boolean,boolean}, {registered(0x16),registered(0x17)},
      {registered(0x11),registered(0x12)}, {decimal,registered(0x13)},
      {registered(0x14),registered(0x15)}, {registered(0x18),registered(0x19)}}};
  const auto rows = dt::CurrentDatatypeTypeCodecIdentityRowsV1();
  Require(rows.size() == expected.size(), "registry population changed without an admitted oracle");
  for (const auto& [descriptor,type] : expected) {
    const auto result = dt::LookupDatatypeTypeCodecIdentityV1(snapshot,1,1,descriptor,1);
    Require(result.ok && result.row.catalog_snapshot_uuid == snapshot &&
            result.row.descriptor_uuid == descriptor && result.row.type_uuid == type,
            "binary registry lookup changed a registered UUID");
    Require(uuid::IsEngineIdentityUuid(descriptor) && uuid::IsEngineIdentityUuid(type),
            "registered system identity is not UUIDv7");
    for (unsigned byte = 0; byte < 16; ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        auto wrong_snapshot=snapshot, wrong_descriptor=descriptor;
        wrong_snapshot.bytes[byte] ^= static_cast<unsigned char>(1u << bit);
        wrong_descriptor.bytes[byte] ^= static_cast<unsigned char>(1u << bit);
        Require(!dt::LookupDatatypeTypeCodecIdentityV1(wrong_snapshot,1,1,descriptor,1).ok,
                "registry ignored a snapshot UUID byte");
        const bool another_registered = std::ranges::any_of(expected, [&](const auto& entry) {
          return entry.first == wrong_descriptor;
        });
        const auto changed=dt::LookupDatatypeTypeCodecIdentityV1(snapshot,1,1,wrong_descriptor,1);
        Require(changed.ok == another_registered,
                "registry ignored a descriptor UUID byte or confused adjacent registered IDs");
      }
    }
    for (std::uint64_t generation : {0ULL,2ULL,~0ULL}) {
      Require(!dt::LookupDatatypeTypeCodecIdentityV1(snapshot,generation,1,descriptor,1).ok &&
              !dt::LookupDatatypeTypeCodecIdentityV1(snapshot,1,generation,descriptor,1).ok &&
              !dt::LookupDatatypeTypeCodecIdentityV1(snapshot,1,1,descriptor,generation).ok,
              "binary identity admitted a stale generation");
    }
  }
  const auto text = dt::LookupDatatypeTypeCodecIdentityV1(snapshot,1,1,registered(0x18),1);
  Require(text.ok && text.row.codec_uuid == registered(0x1a) &&
          dt::IsExactCanonicalTextTypeCodecIdentityV1(text.row), "TEXT codec UUID changed");
  Require(dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
              boolean,1,boolean,1,"datatype.boolean.u8.v1",1,1,true) &&
          !dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
              boolean,1,boolean,1,"datatype.boolean.u8.v1",1,1,false),
          "binary conversion weakened the boolean identity exception");
  std::cout << "PASS six binary datatype registry rows, exact byte/generation lookup and boolean/TEXT identities\n";
}

// Independent byte oracle, not the production comparator or host integer order.
int Oracle(const Uuid& a, const Uuid& b) {
  for (unsigned i = 0; i < 16; ++i) {
    if (a.bytes[i] != b.bytes[i]) return a.bytes[i] < b.bytes[i] ? -1 : 1;
  }
  return 0;
}

NOINLINE int ArrayCompare(const Uuid& a, const Uuid& b) {
  if (a.bytes < b.bytes) return -1;
  if (b.bytes < a.bytes) return 1;
  return 0;
}

NOINLINE int ByteCompare(const Uuid& a, const Uuid& b) {
  const int result = std::memcmp(a.bytes.data(), b.bytes.data(), 16);
  return (result > 0) - (result < 0);
}

std::uint64_t Step(std::uint64_t& state) {
  state ^= state << 13; state ^= state >> 7; state ^= state << 17;
  return state;
}

std::vector<Uuid> Keys(std::size_t count, bool common_prefix) {
  std::uint64_t state = 0x6d7b924a83519e01ull;
  std::vector<Uuid> keys(count);
  for (std::size_t i = 0; i < count; ++i) {
    for (auto& byte : keys[i].bytes) byte = static_cast<unsigned char>(Step(state));
    if (common_prefix) {
      for (unsigned j = 0; j < 6; ++j) keys[i].bytes[j] = static_cast<unsigned char>(j + 1);
      keys[i].bytes[6] = static_cast<unsigned char>((keys[i].bytes[6] & 15) | 0x70);
    }
    // Unique low 64 bits, encoded explicitly in most-significant-byte order.
    for (unsigned j = 0; j < 8; ++j) keys[i].bytes[15-j] =
        static_cast<unsigned char>(static_cast<std::uint64_t>(i) >> (j*8));
  }
  return keys;
}

template<int (*Compare)(const Uuid&, const Uuid&)>
struct BytePolicy {
  using Key = Uuid;
  static Key Pack(const Uuid& value) { return value; }
  static Uuid Unpack(const Key& value) { return value; }
  static int Cmp(const Key& a, const Key& b) { return Compare(a,b); }
};

#if defined(__SIZEOF_INT128__)
struct IntegerPolicy {
  using Key = unsigned __int128;
  static Key Pack(const Uuid& value) {
    Key key = 0;
    for (auto byte : value.bytes) key = (key << 8) | byte;
    return key;
  }
  static Uuid Unpack(Key value) {
    Uuid result;
    for (unsigned i = 0; i < 16; ++i) { result.bytes[15-i] = value & 255; value >>= 8; }
    return result;
  }
  static NOINLINE int Cmp(const Key& a, const Key& b) { return (a > b) - (a < b); }
};
#endif

// Identical canonical-byte hash for both representations; ephemeral benchmark
// only, not a durable catalog/index hash or security digest.
std::size_t HashBytes(const Uuid& value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (auto byte : value.bytes) { hash ^= byte; hash *= 1099511628211ull; }
  return static_cast<std::size_t>(hash);
}

template<class Policy> struct Less {
  bool operator()(const typename Policy::Key& a, const typename Policy::Key& b) const {
    return Policy::Cmp(a,b) < 0;
  }
};
template<class Policy> struct Hash {
  std::size_t operator()(const typename Policy::Key& value) const { return HashBytes(Policy::Unpack(value)); }
};
template<class Policy> struct Equal {
  bool operator()(const typename Policy::Key& a, const typename Policy::Key& b) const { return Policy::Cmp(a,b) == 0; }
};

void Contract() {
  CatalogIdentityContract();
  std::uint64_t comparisons = 0;
  for (unsigned position = 0; position < 16; ++position) {
    for (unsigned left = 0; left < 256; ++left) for (unsigned right = 0; right < 256; ++right) {
      Uuid a, b;
      a.bytes.fill(0x55); b.bytes.fill(0x55);
      a.bytes[position] = left; b.bytes[position] = right;
      const auto expected = Oracle(a,b);
      Require(uuid::CompareUuid128(a,b) == expected, "production byte ordering mismatch");
      Require((a < b) == (expected < 0) && (a > b) == (expected > 0) &&
              (a <= b) == (expected <= 0) && (a >= b) == (expected >= 0),
              "binary engine/container ordering differs from the byte oracle");
      Require(ArrayCompare(a,b) == expected && ByteCompare(a,b) == expected, "candidate ordering mismatch");
#if defined(__SIZEOF_INT128__)
      const auto packed_a = IntegerPolicy::Pack(a), packed_b = IntegerPolicy::Pack(b);
      Require(IntegerPolicy::Cmp(packed_a,packed_b) == expected, "integer ordering mismatch");
      Require(IntegerPolicy::Unpack(packed_a) == a, "integer canonical-byte roundtrip failed");
#endif
      ++comparisons;
    }
  }
  const auto keys = Keys(4096, false);
  for (const auto& value : keys) {
    Require(scratchbird::engine::internal_api::EngineUuidHash{}(value) == HashBytes(value),
            "engine key hash differs from the independent canonical-byte oracle");
    // Binary keys must not be mistaken for native-endian integer payload bytes.
    unsigned char unaligned[18]{};
    std::memcpy(unaligned+1, value.bytes.data(), 16);
    Uuid restored;
    std::memcpy(restored.bytes.data(), unaligned+1, 16);
    Require(restored == value && HashBytes(restored) == HashBytes(value), "unaligned binary roundtrip failed");
    Require(uuid::CompareUuid128(value,value) == 0, "equality ordering failed");
    for (unsigned version = 1; version <= 7; ++version) {
      auto user = value;
      user.bytes[6] = static_cast<unsigned char>((user.bytes[6] & 15) | (version << 4));
      user.bytes[8] = static_cast<unsigned char>((user.bytes[8] & 63) | 0x80);
      Require(uuid::UuidVersionAllowed(user, {}), "older user UUID refused");
      Require(uuid::IsEngineIdentityUuid(user) == (version == 7), "user UUID became system authority");
      Require(uuid::CompareUuid128(user,value) == Oracle(user,value), "version-dependent comparator");
    }
  }
  std::cout << "PASS binary16 layout, " << comparisons
            << " byte-order comparisons, 4096 unaligned roundtrips and user/system version separation\n";
}

template<class Work>
void Measure(const char* dataset, const char* representation, unsigned repetition,
             const char* operation, Work work) {
  const auto start = Clock::now();
  const auto checksum = work();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
  std::cout << dataset << ',' << representation << ',' << repetition << ',' << operation
            << ',' << elapsed << ',' << checksum << '\n';
}

template<class Policy>
void Benchmark(const std::vector<Uuid>& source, const char* dataset, const char* name, unsigned repetition) {
  using Key = typename Policy::Key;
  std::vector<Key> keys;
  Measure(dataset,name,repetition,"pack",[&] {
    keys.reserve(source.size());
    for (const auto& value : source) keys.push_back(Policy::Pack(value));
    return keys.size();
  });
  Measure(dataset,name,repetition,"compare",[&] {
    std::uint64_t count=0;
    for (unsigned repeat=0; repeat<32; ++repeat)
      for (std::size_t i=1; i<keys.size(); ++i) count += Policy::Cmp(keys[i-1],keys[i]) + 1;
    return count;
  });
  auto ordered = keys;
  Measure(dataset,name,repetition,"sort",[&] {
    std::sort(ordered.begin(),ordered.end(),Less<Policy>{}); return ordered.size();
  });
  for (std::size_t i=1; i<ordered.size(); ++i)
    Require(Oracle(Policy::Unpack(ordered[i-1]),Policy::Unpack(ordered[i])) < 0, "sort oracle failed");
  std::map<Key,std::size_t,Less<Policy>> index;
  Measure(dataset,name,repetition,"ordered_insert",[&] {
    for (std::size_t i=0; i<keys.size(); ++i) index.emplace(keys[i],i);
    Require(index.size()==keys.size(),"ordered insert lost a key"); return index.size();
  });
  Measure(dataset,name,repetition,"ordered_lookup",[&] {
    std::uint64_t sum=0;
    for (std::size_t i=0; i<keys.size(); ++i) { const auto it=index.find(keys[i]); Require(it!=index.end()&&it->second==i,"lookup mismatch"); sum+=it->second; }
    return sum;
  });
  Measure(dataset,name,repetition,"range_scan",[&] {
    std::uint64_t sum=0;
    for (std::size_t i=0; i<ordered.size(); i+=64) {
      auto it=index.lower_bound(ordered[i]);
      for (unsigned j=0; j<32 && it!=index.end(); ++j,++it) sum+=it->second;
    }
    return sum;
  });
  std::unordered_map<Key,std::size_t,Hash<Policy>,Equal<Policy>> hashed;
  hashed.reserve(keys.size());
  Measure(dataset,name,repetition,"hash_insert",[&] {
    for (std::size_t i=0; i<keys.size(); ++i) hashed.emplace(keys[i],i);
    Require(hashed.size()==keys.size(),"hash insert lost a key");return hashed.size();
  });
  Measure(dataset,name,repetition,"hash_lookup",[&] {
    std::uint64_t sum=0;
    for (std::size_t i=0; i<keys.size(); ++i) {const auto it=hashed.find(keys[i]);Require(it!=hashed.end()&&it->second==i,"hash lookup mismatch");sum+=it->second;}
    return sum;
  });
}

void CanonicalSortBinaryIdentityContract() {
  namespace query = scratchbird::engine::sblr;
  namespace api = scratchbird::engine::internal_api;
  static_assert(std::is_same_v<decltype(query::PreparedSortRoot::ordering_property_uuid),Uuid>);
  const auto id=[](unsigned tail) {
    return Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0,static_cast<std::uint8_t>(tail)}};
  };
  const auto order=id(1),tie=id(2),descriptor_id=id(3),type=id(4),collation=id(5);
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid=descriptor_id;
  descriptor.type_uuid=type;
  descriptor.collation_uuid=collation;
  unsigned checks=0;
  const auto check=[&](bool actual,bool expected) {
    ++checks; Require(actual==expected,"sort binary identity binding admitted a wrong identity");
  };
  const auto bound=[&](const api::EngineDescriptor& d,Uuid evidence,Uuid property,Uuid coll) {
    return query::CanonicalSortExpressionIdentityBinding(property,evidence,d,coll);
  };
  const auto exact=[&](const api::EngineDescriptor& d){return bound(d,tie,order,collation);};
  check(exact(descriptor),true);
  auto copied=descriptor; copied.encoded_descriptor="type_uuid=019d0000-0000-5000-8000-000000000099";
  // This identity-only guard must not parse a text shadow. Full descriptor
  // validity remains a separate mandatory check in receipt issuance.
  check(exact(copied),true);
  copied.type_uuid={}; check(exact(copied),false);
  copied=descriptor; copied.collation_uuid={}; check(exact(copied),false);
  check(bound(copied,tie,order,{}),true);
  for(const auto alias:{order,descriptor_id,type,collation,Uuid{}})
    check(bound(descriptor,alias,order,collation),false);
  check(bound(descriptor,tie,{},collation),false);
  for(unsigned version=0;version<16;++version) {
    for(unsigned slot=0;slot<5;++slot) {
      auto altered=descriptor; auto property=order,evidence=tie,coll=collation;
      Uuid* identities[]={&property,&evidence,&altered.descriptor_uuid,&altered.type_uuid,&coll};
      identities[slot]->bytes[6]=static_cast<std::uint8_t>(version<<4);
      if(slot==4)altered.collation_uuid=coll;
      check(bound(altered,evidence,property,coll),version==7);
    }
  }
  for(unsigned variant=0;variant<4;++variant) {
    for(unsigned slot=0;slot<5;++slot) {
      auto altered=descriptor; auto property=order,evidence=tie,coll=collation;
      Uuid* identities[]={&property,&evidence,&altered.descriptor_uuid,&altered.type_uuid,&coll};
      identities[slot]->bytes[8]=static_cast<std::uint8_t>(variant<<6);
      if(slot==4)altered.collation_uuid=coll;
      check(bound(altered,evidence,property,coll),variant==2);
    }
  }
  // Distinct raw bytes remain distinct even when textual descriptor metadata
  // or a shared prefix would otherwise collapse the identity.
  for(unsigned byte=0;byte<16;++byte) {
    auto changed=tie; changed.bytes[15]=0x60; changed.bytes[byte]^=1;
    check(bound(descriptor,changed,order,collation),true);
  }
  std::cout << "canonical_sort_binary_identity checks=" << checks << '\n';
}

int main(int argc, char** argv) {
  try {
    const auto epoch_millis = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
    };
    const auto issuance_begin = epoch_millis();
    std::map<Uuid, std::uint64_t> issued_identities;
    for (unsigned index = 0; index < 1024; ++index) {
      const auto identity = scratchbird::engine::executor::IssueRuntimeIdentityV7();
      Require(identity.has_value() && uuid::IsEngineIdentityUuid(*identity),
              "runtime issuer did not return a binary system UUIDv7");
      std::uint64_t raw_millis = 0;
      for (unsigned byte = 0; byte < 6; ++byte)
        raw_millis = raw_millis * 256 + identity->bytes[byte];
      Require(issued_identities.emplace(*identity, raw_millis).second,
              "runtime issuer reused an identity");
    }
    const auto issuance_end = epoch_millis();
    for (const auto& [identity, millis] : issued_identities) {
      Require(millis >= static_cast<std::uint64_t>(issuance_begin) &&
              millis <= static_cast<std::uint64_t>(issuance_end),
              "runtime UUIDv7 prefix is not its actual wall-clock issuance time");
    }
    const Uuid path_identity{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x14}};
    const auto component = uuid::EngineIdentityPathComponent(path_identity);
    Require(component.has_value() && component->generic_string() ==
                "019d0000-0000-7000-8000-00000000d714" &&
                !component->has_parent_path() && !component->is_absolute(),
            "binary identity filesystem projection is not one exact safe component");
    Require(!uuid::EngineIdentityPathComponent({}).has_value(),
            "nil identity gained an owner path");
    for (unsigned version = 0; version < 16; ++version) {
      auto changed = path_identity;
      changed.bytes[6] = static_cast<std::uint8_t>(version << 4);
      Require(uuid::EngineIdentityPathComponent(changed).has_value() == (version == 7),
              "non-v7 system identity gained an owner path");
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
      auto changed = path_identity;
      changed.bytes[8] = static_cast<std::uint8_t>(variant << 6);
      Require(uuid::EngineIdentityPathComponent(changed).has_value() == (variant == 2),
              "invalid system variant gained an owner path");
    }
    CurrentRowBinaryIdentityContract();
    RelationMetadataBinaryIdentityContract();
    HeapBinaryMemoryContract();
    HeapBinaryIdentityCodecContract();
    HeapBinaryFieldsContract();
    CanonicalSortBinaryIdentityContract();
    TypedDescriptorBinaryIdentityContract();
    AggregateRegistryBinaryIdentityContract();
    BinaryIdentityDecodeContract();
    PublicResolutionCacheIdentityContract();
    Contract();
    if (argc==1) return 0;
    Require(argc==2 && std::string(argv[1])=="--benchmark","invalid benchmark arguments");
    std::cout << "layout,binary16," << sizeof(Uuid) << ',' << alignof(Uuid) << '\n';
#if defined(__SIZEOF_INT128__)
    std::cout << "layout,uint128," << sizeof(IntegerPolicy::Key) << ',' << alignof(IntegerPolicy::Key) << '\n';
#endif
    std::cout << "dataset,representation,repetition,operation,nanoseconds,checksum\n";
    for (bool common : {false,true}) {
      const auto keys=Keys(32768,common);const char* dataset=common?"common_prefix":"random";
      for (unsigned repeat=0;repeat<5;++repeat) {
        // Rotate order to avoid assigning a fixed warm-cache advantage.
        for (unsigned slot=0;slot<3;++slot) switch ((repeat+slot)%3) {
          case 0: Benchmark<BytePolicy<ArrayCompare>>(keys,dataset,"array16",repeat);break;
          case 1: Benchmark<BytePolicy<ByteCompare>>(keys,dataset,"memcmp16",repeat);break;
          case 2:
#if defined(__SIZEOF_INT128__)
            Benchmark<IntegerPolicy>(keys,dataset,"uint128",repeat);
#endif
            break;
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';return 1;
  }
}
