// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_index_order.hpp"
#include "index_key_encoding.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace b = api::dml::detail;
namespace idx = scratchbird::core::index;
namespace p = scratchbird::core::platform;
namespace {
unsigned checks = 0;
void Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) throw std::runtime_error(reason);
}
api::EngineUuid Id(unsigned tag) {
  api::EngineUuid id{{1,2,3,4,5,6,0x70,8,0x80,0,0,0,0,0,0,0}};
  id.bytes[10] = static_cast<p::byte>(tag);
  return id;
}
std::string Key(unsigned value, unsigned mode) {
  if (mode == 0) return std::string{char(0), char(value), char(0xff)};
  idx::IndexKeyEncodingComponent component;
  component.ordinal = 0;
  component.type_descriptor_uuid = {p::UuidKind::object, Id(23)};
  component.payload = {0, static_cast<p::byte>(value), 0xff};
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Check(encoded.ok(), "canonical key fixture encoding failed");
  std::string key(encoded.encoded.begin(), encoded.encoded.end());
  return mode == 1 ? std::string("SBKOBIN:") + key : key;
}
b::DirectPrecomputedIndexEntry Entry(unsigned key, unsigned row, unsigned version,
                                    unsigned ordinal, unsigned mode) {
  return {Key(key, mode), std::string("payload\0\xff", 9), Id(row), Id(version), ordinal, false};
}
bool Same(const b::DirectPrecomputedIndexEntry& a, const b::DirectPrecomputedIndexEntry& z) {
  return a.encoded_key == z.encoded_key && a.payload_value == z.payload_value &&
      a.row_uuid == z.row_uuid && a.version_uuid == z.version_uuid &&
      a.source_ordinal == z.source_ordinal && a.null_key == z.null_key;
}
void Ordering() {
  for (unsigned mode=0; mode<3; ++mode) {
    // Exact independent order: key, then all raw row bytes, then version bytes,
    // then source ordinal. High-bit UUID bytes expose signed/text comparison bugs.
    const std::vector<b::DirectPrecomputedIndexEntry> expected{
        Entry(0,0xff,0xff,9,mode), Entry(0x80,0x7f,0xff,9,mode),
        Entry(0x80,0x80,0x7f,9,mode), Entry(0x80,0x80,0x80,1,mode),
        Entry(0x80,0x80,0x80,2,mode), Entry(0xff,0,0,0,mode)};
    for (std::size_t i=0; i<expected.size(); ++i)
      for (std::size_t j=0; j<expected.size(); ++j) {
        Check(b::DirectPrecomputedIndexEntryLess(expected[i],expected[j]) == (i<j), "binary comparator order changed");
        Check(b::DirectPrecomputedIndexEntryExactAppendLess(expected[i],expected[j]) == (i<j), "exact append order changed");
        Check(b::DirectPrecomputedIndexEntryTextLess(expected[i],expected[j]) == (i<j), "byte-string order changed");
      }
    std::array<unsigned,6> permutation{0,1,2,3,4,5};
    do {
      b::DirectPrecomputedIndexEntryMap map;
      for (auto i:permutation) map[Id(1)].push_back(expected[i]);
      const auto untouched=Entry(7,4,5,6,mode);
      map[Id(2)]={untouched};
      auto exact=map[Id(1)];
      b::DirectSortPrecomputedIndexEntries(&map);
      b::DirectSortPrecomputedIndexEntriesForExactAppend(&exact);
      Check(map[Id(1)].size()==expected.size() && exact.size()==expected.size(), "sort lost entries");
      for (std::size_t i=0; i<expected.size(); ++i) {
        Check(Same(map[Id(1)][i],expected[i]), "sort detached payload or UUID from key");
        Check(Same(exact[i],expected[i]), "exact sort detached payload or UUID from key");
      }
      Check(Same(map[Id(2)][0],untouched), "sort crossed binary index ownership");
      Check(b::DirectPrecomputedEntriesForIndex(&map,Id(1)) == &map.at(Id(1)), "binary index lookup copied or missed entries");
      Check(b::DirectPrecomputedEntriesForIndex(&map,Id(3)) == nullptr, "foreign index lookup returned entries");
      Check(b::DirectPrecomputedEntriesHaveDuplicateKeys(map[Id(1)]), "duplicate key detection incorrectly includes row UUID");
    } while (std::next_permutation(permutation.begin(),permutation.end()));
    Check(b::DirectPrecomputedEntriesRequireEncodedCompare(expected) == (mode!=0), "key format comparison selection changed");
  }
}
void Tracking() {
  const auto index=Id(1);
  const auto first=Entry(1,0,0,1,1), second=Entry(2,0,0,2,1);
  b::DirectPrecomputedIndexEntryMap rows{{index,{first,second}}};
  b::DirectPrecomputedIndexEntryOrderStateMap states;
  Check(!b::DirectPrecomputedIndexEntriesAppendOrderSorted(rows,states), "missing order evidence accepted");
  Check(!b::DirectPrecomputedIndexEntriesDuplicateKeysAbsent(rows,states), "missing duplicate evidence accepted");
  b::DirectTrackPrecomputedIndexEntryOrder(index,first,&states);
  b::DirectTrackPrecomputedIndexEntryOrder(index,second,&states);
  Check(b::DirectPrecomputedIndexEntriesAppendOrderSorted(rows,states), "ordered sequence rejected");
  Check(b::DirectPrecomputedIndexEntriesDuplicateKeysAbsent(rows,states), "distinct keys rejected");
  Check(!b::DirectPrecomputedEntriesHaveDuplicateKeys(rows[index]), "distinct key vector rejected");
  b::DirectTrackPrecomputedIndexEntryOrder(index,first,&states);
  Check(!b::DirectPrecomputedIndexEntriesAppendOrderSorted(rows,states), "descending insertion not detected");
  auto duplicate=first;duplicate.row_uuid=Id(0xff);
  b::DirectTrackPrecomputedIndexEntryOrder(index,duplicate,&states);
  Check(!b::DirectPrecomputedIndexEntriesDuplicateKeysAbsent(rows,states), "different row UUID hid duplicate key");
  b::DirectTrackPrecomputedIndexEntryOrder(index,second,&states);
  Check(!b::DirectPrecomputedIndexEntriesAppendOrderSorted(rows,states) &&
        !b::DirectPrecomputedIndexEntriesDuplicateKeysAbsent(rows,states), "later row erased prior failure evidence");
  auto a=first,z=first;a.payload_value="first";z.payload_value="second";
  std::vector<b::DirectPrecomputedIndexEntry> equivalent{a,z};
  b::DirectSortPrecomputedIndexEntriesForExactAppend(&equivalent);
  Check(equivalent[0].payload_value=="first"&&equivalent[1].payload_value=="second", "exact-equivalent entries lost stable order");
  b::DirectSortPrecomputedIndexEntries(nullptr);
  b::DirectSortPrecomputedIndexEntriesForExactAppend(nullptr);
  b::DirectTrackPrecomputedIndexEntryOrder(index,first,nullptr);
  Check(!b::DirectPrecomputedEntriesForIndex(nullptr,index), "null input lookup returned entries");
  Check(!b::DirectPrecomputedEntriesHaveDuplicateKeys({}) &&
        !b::DirectPrecomputedEntriesHaveDuplicateKeys({first}), "empty/single row reported duplicates");
}
}
int main() {
  try { Ordering(); Tracking(); std::cout<<"direct index ordering checks="<<checks<<'\n'; return 0; }
  catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
