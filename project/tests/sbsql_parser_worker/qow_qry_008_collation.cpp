// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual canonical comparison component; not live receipt or SQL/IPC evidence.
#include "query/expression_api.hpp"
#include "engine/sblr/relational_descriptor_codec.hpp"
#include "canonical_utf8.hpp"
#include "resource_seed_pack.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace { long fail_after = -1; std::size_t checks = 0, faults = 0; }
void* operator new(std::size_t size) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace resources = scratchbird::core::resources;
using Uuid = api::EngineUuid;
namespace {
void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
Uuid Id(std::uint8_t suffix) {
  return {{0x01,0x9f,0,0,0,0,0x74,0,0x80,0,0,0,0,0,8,suffix}};
}
void Number(std::string& body, std::size_t offset, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) body[offset + i] = static_cast<char>(value >> (8 * i));
}
void Identity(std::string& body, std::size_t offset, Uuid uuid) {
  std::copy(uuid.bytes.begin(), uuid.bytes.end(), body.begin() + offset);
}
api::EngineDescriptor TextDescriptor(std::uint8_t suffix) {
  api::EngineDescriptor result;
  result.descriptor_uuid = Id(suffix); result.descriptor_kind = "scalar";
  result.type_uuid = Id(2); result.collation_uuid = Id(1);
  result.canonical_type_name = "text";
  // Independent exact kind213 body, not a call to the production encoder.
  std::string body(152, '\0');
  Number(body, 0, 1, 2); Number(body, 2, 3, 2); Number(body, 4, suffix, 4);
  Identity(body, 8, Id(suffix)); Identity(body, 24, Id(2)); Identity(body, 40, Id(1));
  Identity(body, 56, Id(3)); Identity(body, 72, Id(4));
  for (auto offset : {88,96,104,112,120}) Number(body, offset, 1, 8);
  Number(body, 128, 1, 2); Number(body, 130, 1, 1);
  constexpr std::string_view codec = "datatype.text.utf8.v1";
  Number(body, 144, codec.size(), 4); body.append(codec);
  result.encoded_descriptor = std::move(body);
  return result;
}
api::EngineTypedValue Value(std::string bytes, std::uint8_t suffix = 5) {
  api::EngineTypedValue value; value.descriptor = TextDescriptor(suffix);
  value.encoded_value = std::move(bytes); value.state = api::EngineValueState::value;
  return value;
}
dt::DatatypeTextSeedAuthority Seed() {
  // Real qualified immutable input; fixture UUIDs below are component bindings,
  // not a claim of live transaction/catalog or SQL/IPC admission.
  static const auto image = [] {
    resources::ResourceSeedLoadConfig config;
    config.seed_pack_root = SB_COLLATION_TEST_SEED_ROOT;
    auto loaded = resources::LoadResourceSeedPack(config);
    Require(loaded.ok() && loaded.image.unicode_collation, "qualified collation input failed admission");
    return std::move(loaded.image);
  }();
  dt::DatatypeTextSeedAuthority result;
  result.database_uuid=Id(9);result.charset_uuid=Id(8);result.collation_uuid=Id(1);
  result.resource_epoch=31;result.collation_epoch=17;
  result.comparison_profile=resources::CollationProfile::uca17_root_primary;
  result.unicode_collation=image.unicode_collation;
  result.active = true; result.seed_pack_name = "qow_core_resource_catalog";
  result.seed_pack_version = "2026.07"; result.charset_name = "UTF-8";
  result.collation_name = "unicode_ci_ai";
  result.collation_case_insensitive = true; result.collation_accent_insensitive = true;
  return result;
}
void Compare(std::string a, std::string b, const dt::DatatypeTextSeedAuthority& seed, int sign) {
  auto left = Value(std::move(a)); auto right = Value(std::move(b), 6);
  int result = 99; std::string refusal;
  Require(api::QowCompareCanonicalCollatedScalarsV1(
      left, right, Id(1), 31, 17, seed, &result, &refusal), "canonical comparison refused");
  Require(refusal.empty() && ((result > 0) - (result < 0)) == sign, "incorrect collation result");
}
void TestComparison() {
  Compare("R\xc3\xa9sum\xc3\xa9", "resume", Seed(), 0);
  Compare("alpha", "Beta", Seed(), -1);
  Compare("z", "a", Seed(), 1); Compare("", "", Seed(), 0); Compare("", "x", Seed(), -1);
  auto exact = Seed(); exact.collation_case_insensitive = false;
  exact.collation_accent_insensitive = false; exact.collation_name = "unicode_cs";
  exact.comparison_profile=resources::CollationProfile::utf8_binary;
  exact.unicode_collation.reset();
  Compare("A", "a", exact, -1);
}
void TestUnicodeComparison() {
  // These are full Unicode case/accent requirements, not Latin-only examples.
  Compare("\xce\x91", "\xce\xb1", Seed(), 0);  // Greek alpha
  Compare("\xce\xa3", "\xcf\x82", Seed(), 0);  // Greek final sigma
  Compare("\xd0\x90", "\xd0\xb0", Seed(), 0);  // Cyrillic a
  Compare("e\xcc\x81", "e", Seed(), 0);         // combining acute
}
void TestKeysAndHashes() {
  const std::vector<std::pair<std::string,std::string>> pairs{
      {"A","a"},{"e","\xc3\xa9"},{"\xc3\xa9","e\xcc\x81"},
      {"\xce\x91","\xce\xb1"},{"\xce\xa3","\xcf\x82"},
      {"Stra\xc3\x9f" "e","STRASSE"},{std::string("a\0b",3),"a"},
      {"",""},{"", "x"},{"\xea\xb0\x81","\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8"}};
  for(unsigned profile=1;profile<=5;++profile) {
    auto seed=Seed();seed.comparison_profile=static_cast<resources::CollationProfile>(profile);
    seed.collation_case_insensitive=profile==2||profile==3;
    seed.collation_accent_insensitive=profile==2;
    if(profile==1)seed.unicode_collation.reset();
    for(std::size_t i=0;i<pairs.size();++i) {
      dt::DatatypeComparisonRequest compare;
      compare.left={dt::CanonicalTypeId::character,pairs[i].first,false};
      compare.right={dt::CanonicalTypeId::character,pairs[i].second,false};compare.text_seed=seed;
      const auto order=dt::CompareDatatypeValues(compare);Require(order.ok(),"profile comparison failed");
      dt::DatatypeSortKeyRequest sort;sort.text_seed=seed;sort.value=compare.left;
      const auto a=dt::MakeDatatypeSortKey(sort);sort.value=compare.right;
      const auto b=dt::MakeDatatypeSortKey(sort);
      Require(a.ok()&&b.ok(),"profile sort failed");
      const int sign=a.sort_key<b.sort_key?-1:a.sort_key>b.sort_key?1:0;
      Require(sign==order.comparison,"sort/compare equivalence drift");
      dt::DatatypeHashRequest hash;hash.text_seed=seed;hash.value=compare.left;
      const auto x=dt::HashDatatypeValue(hash);hash.value=compare.right;
      const auto y=dt::HashDatatypeValue(hash);
      Require(x.ok()&&y.ok(),"profile hash failed");
      if(order.comparison==0)Require(x.stable_hash_hex==y.stable_hash_hex,"equal TEXT hashed differently");
      if(i==0)Require((order.comparison==0)==(profile==2||profile==3),"wrong case strength");
      if(i==1)Require((order.comparison==0)==(profile==2),"wrong accent strength");
      if(i==2||i==9)Require((order.comparison==0)==(profile!=1),"wrong canonical equivalence");
      auto renamed=seed;renamed.seed_pack_name.clear();renamed.seed_pack_version.clear();
      renamed.charset_name="renamed";renamed.collation_name.clear();
      sort.text_seed=renamed;hash.text_seed=renamed;
      const auto renamed_key=dt::MakeDatatypeSortKey(sort);const auto renamed_hash=dt::HashDatatypeValue(hash);
      Require(renamed_key.ok()&&renamed_key.sort_key==b.sort_key&&renamed_hash.ok()&&
          renamed_hash.stable_hash_hex==y.stable_hash_hex,"display metadata changed key authority");
      if(profile==1) {
        // Independent Core prefix oracle: raw identities, then three LE u64s.
        std::string expected="20:";
        for(auto id:{Id(9),Id(8),Id(1)})expected.append(reinterpret_cast<const char*>(id.bytes.data()),16);
        for(auto number:{31u,17u,1u})for(unsigned n=0;n<8;++n)
          expected.push_back(static_cast<char>(std::uint64_t(number)>>(8*n)));
        expected+=pairs[i].first;
        Require(a.sort_key==expected,"binary cohort key byte oracle mismatch");
      }
    }
  }
  dt::DatatypeOperationValue text{dt::CanonicalTypeId::character,"valid",false};
  dt::DatatypeComparisonRequest comparison;comparison.left=text;comparison.right=text;
  dt::DatatypeSortKeyRequest sort;sort.value=text;
  dt::DatatypeHashRequest hash;hash.value=text;
  Require(!dt::CompareDatatypeValues(comparison).ok(),"unbound TEXT comparison silently used binary");
  Require(!dt::MakeDatatypeSortKey(sort).ok(),"unbound TEXT sort silently used binary");
  Require(!dt::HashDatatypeValue(hash).ok(),"unbound TEXT hash silently used spelling");
}
void TestRefusal() {
  const auto left = Value("alpha"); const auto right = Value("ALPHA", 6);
  const auto refuse = [&](const api::EngineTypedValue& a, const api::EngineTypedValue& b,
                          const Uuid& identity, std::uint64_t epoch, std::uint64_t family,
                          const dt::DatatypeTextSeedAuthority& seed) {
    int result = 99; std::string refusal = "old";
    Require(!api::QowCompareCanonicalCollatedScalarsV1(
        a, b, identity, epoch, family, seed, &result, &refusal) &&
        result == 99 && !refusal.empty(), "refusal published comparison or accepted malformed input");
  };
  refuse(left, right, Uuid{}, 31, 17, Seed());
  for (std::size_t i = 0; i < 16; ++i) {
    auto id = Id(1); id.bytes[i] ^= 1; refuse(left, right, id, 31, 17, Seed());
  }
  refuse(left, right, Id(1), 0, 17, Seed()); refuse(left, right, Id(1), 31, 0, Seed());
  for (auto member : {&dt::DatatypeTextSeedAuthority::database_uuid,
                     &dt::DatatypeTextSeedAuthority::charset_uuid,
                     &dt::DatatypeTextSeedAuthority::collation_uuid}) {
    auto seed = Seed(); (seed.*member)={}; refuse(left, right, Id(1), 31, 17, seed);
    seed=Seed();(seed.*member).bytes[6]=0x40;refuse(left,right,Id(1),31,17,seed);
  }
  auto wrong=Seed();++wrong.resource_epoch;refuse(left,right,Id(1),31,17,wrong);
  wrong=Seed();++wrong.collation_epoch;refuse(left,right,Id(1),31,17,wrong);
  wrong=Seed();wrong.unicode_collation.reset();refuse(left,right,Id(1),31,17,wrong);
  wrong=Seed();wrong.comparison_profile=resources::CollationProfile::unbound;
  refuse(left,right,Id(1),31,17,wrong);
  wrong=Seed();wrong.comparison_profile=static_cast<resources::CollationProfile>(6);
  refuse(left,right,Id(1),31,17,wrong);
  auto seed = Seed(); seed.active = false; refuse(left, right, Id(1), 31, 17, seed);
  for (bool mutate_left : {false,true}) {
    const auto changed_value = [&](const api::EngineTypedValue& value) {
      refuse(mutate_left ? value : left, mutate_left ? right : value, Id(1), 31, 17, Seed());
    };
    auto v = left; v.descriptor.descriptor_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.type_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.collation_uuid = Id(99); changed_value(v);
    v = left; v.descriptor.type_uuid = {}; changed_value(v);
    v = left; v.descriptor.collation_uuid = {}; changed_value(v);
    v = left; v.descriptor.canonical_type_name = "bigint"; changed_value(v);
    v = left; v.descriptor.descriptor_kind = "row"; changed_value(v);
    v = left; v.descriptor.encoded_descriptor = "type_uuid=019f0000-0000-7400-8000-000000000842;collation_uuid=019f0000-0000-7400-8000-000000000801"; changed_value(v);
    v = left; v.descriptor.encoded_descriptor.push_back('\0'); changed_value(v);
    for (std::size_t size = 0; size < left.descriptor.encoded_descriptor.size(); ++size) {
      v = left; v.descriptor.encoded_descriptor.resize(size); changed_value(v);
    }
    for (auto offset : {0,2,4,8,24,40,56,72,88,96,104,112,120,128,130,131,132,136,140,144,148}) {
      v = left;
      if (offset == 8 || offset == 24 || offset == 40 || offset == 56 || offset == 72)
        v.descriptor.encoded_descriptor[offset + 6] = 0x40;
      else Number(v.descriptor.encoded_descriptor, offset, offset == 131 || offset == 132 || offset == 136 || offset == 140 || offset == 148 ? 1 : 0, offset == 130 || offset == 131 ? 1 : 2);
      changed_value(v);
    }
    v = left; v.descriptor.encoded_descriptor[152] = 'X'; changed_value(v);
    for (const auto state : {api::EngineValueState::missing, api::EngineValueState::default_requested,
                            api::EngineValueState::unknown, api::EngineValueState::error,
                            api::EngineValueState::lob_handle, api::EngineValueState::protected_value}) {
      v = left; v.state = state; changed_value(v);
    }
    v = left; v.state = api::EngineValueState::sql_null; v.is_null = true; changed_value(v);
    v = left; v.is_null = true; changed_value(v);
    v = left; v.binary_value.push_back(1); changed_value(v);
  }
  int result = 99; std::string refusal;
  Require(!api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,Seed(),nullptr,&refusal), "null output accepted");
  Require(!api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,Seed(),&result,nullptr) && result == 99, "null refusal accepted");
}
void TestAtomicity() {
  auto left = Value(std::string(200, 'A')), right = Value(std::string(200, 'a'), 6);
  auto seed = Seed(); bool completed = false;
  for (long site = 0; site < 1000; ++site) {
    int comparison = 99; std::string refusal; fail_after = site;
    try {
      const bool ok = api::QowCompareCanonicalCollatedScalarsV1(left,right,Id(1),31,17,seed,&comparison,&refusal);
      fail_after = -1;
      if (ok) { Require(comparison == 0, "allocation sweep returned wrong result"); completed = true; break; }
      Require(comparison == 99 && !refusal.empty(), "allocation refusal published result");
      ++faults;
    } catch (const std::bad_alloc&) {
      fail_after = -1; ++faults;
      Require(comparison == 99, "allocation failure published comparison");
    }
  }
  Require(completed && faults > 0, "allocation sweep incomplete");
  api::RelationalTypeDescriptor original;
  Require(api::QowDecodeCanonicalTextDescriptorV1(left.descriptor,&original), "valid descriptor decode refused");
  auto changed = left.descriptor; changed.encoded_descriptor.push_back('x');
  auto sentinel = original;
  Require(!api::QowDecodeCanonicalTextDescriptorV1(changed,&sentinel) && sentinel == original,
          "failed descriptor decode modified output");
}
std::string Utf8Scalar(std::uint32_t cp) {
  std::string bytes;
  if (cp < 0x80) bytes.push_back(static_cast<char>(cp));
  else if (cp < 0x800) {
    bytes.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    bytes.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else if (cp < 0x10000) {
    bytes.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    bytes.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    bytes.push_back(static_cast<char>(0x80 | (cp & 63)));
  } else {
    bytes.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    bytes.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
    bytes.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
    bytes.push_back(static_cast<char>(0x80 | (cp & 63)));
  }
  return bytes;
}
void TestUtf8Admission() {
  const auto validate = [](const std::string& bytes, std::uint64_t* count) {
    return dt::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), count);
  };
  for (std::uint32_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) continue;
    const auto bytes = Utf8Scalar(cp); std::size_t offset = 0;
    std::uint32_t scalar = 0xffffffff; std::uint64_t count = 99;
    Require(dt::DecodeCanonicalUtf8Scalar(reinterpret_cast<const std::uint8_t*>(bytes.data()),
        bytes.size(), &offset, &scalar) && offset == bytes.size() && scalar == cp,
        "Unicode scalar decoder mismatch");
    Require(validate(bytes, &count) && count == 1, "Unicode scalar validation mismatch");
  }
  for (unsigned a = 0; a < 256; ++a) {
    std::uint64_t count = 99; std::string bytes(1, static_cast<char>(a));
    const bool valid = validate(bytes, &count);
    Require(valid == (a < 128) && count == (valid ? 1 : 0), "one-byte encoding oracle mismatch");
    for (unsigned b = 0; b < 256; ++b) {
      bytes.resize(1); bytes.push_back(static_cast<char>(b)); count = 99;
      const bool ascii = a < 128 && b < 128;
      const bool pair = a >= 0xc2 && a <= 0xdf && b >= 0x80 && b <= 0xbf;
      const bool accepted = validate(bytes, &count);
      Require(accepted == (ascii || pair) && count == (ascii ? 2 : pair ? 1 : 0),
              "two-byte encoding oracle mismatch");
    }
  }
  const std::vector<std::string> malformed = {
      "\x80", "\xc0\x80", "\xc1\xbf", "\xe0\x80\xaf", "\xed\xa0\x80",
      "\xed\xbf\xbf", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
      "\xf8\x88\x80\x80\x80", "\xff", "\xc2", "\xe1\x80", "\xf1\x80\x80",
      "\xc2z", "\xe1z\x80", "\xf1\x80z\x80"};
  const auto invalid_operations = [&](const std::string& bytes) {
    dt::DatatypeOperationValue value{dt::CanonicalTypeId::character, bytes, false};
    dt::DatatypeComparisonRequest compare; compare.left = value;
    compare.right = {dt::CanonicalTypeId::character, "valid", false}; compare.text_seed = Seed();
    Require(!dt::CompareDatatypeValues(compare).ok(), "invalid TEXT comparison accepted");
    std::swap(compare.left, compare.right);
    Require(!dt::CompareDatatypeValues(compare).ok(), "invalid right TEXT comparison accepted");
    compare.left.is_null = true;
    Require(!dt::CompareDatatypeValues(compare).ok(), "NULL bypassed invalid non-null TEXT");
    dt::DatatypeSortKeyRequest sort; sort.value = value; sort.text_seed = Seed();
    const auto key = dt::MakeDatatypeSortKey(sort);
    Require(!key.ok() && key.sort_key.empty(), "invalid TEXT produced sort key");
    dt::DatatypeHashRequest hash; hash.value = value;
    const auto hashed = dt::HashDatatypeValue(hash);
    Require(!hashed.ok() && hashed.stable_hash_hex.empty(), "invalid TEXT produced hash");
    dt::DatatypeSerializationRequest serialize; serialize.value = value;
    const auto serialized = dt::SerializeDatatypeValue(serialize);
    Require(!serialized.ok() && serialized.serialized_value.empty(), "invalid TEXT serialized");
    constexpr char hex[] = "0123456789abcdef";
    std::string wire = "SBDV1;type=text;state=value;payload=";
    for (unsigned char byte : bytes) { wire.push_back(hex[byte >> 4]); wire.push_back(hex[byte & 15]); }
    dt::DatatypeDeserializationRequest deserialize; deserialize.serialized_value = wire;
    const auto decoded = dt::DeserializeDatatypeValue(deserialize);
    Require(!decoded.ok() && decoded.value.encoded_value.empty() &&
            decoded.value.type_id == dt::CanonicalTypeId::unknown, "invalid TEXT deserialization published value");
    int result = 99; std::string refusal;
    Require(!api::QowCompareCanonicalCollatedScalarsV1(Value(bytes), Value("valid",6), Id(1),31,17,
        Seed(), &result, &refusal) && result == 99, "invalid TEXT reached successful scalar comparison");
  };
  for (const auto& bytes : malformed) {
    std::size_t offset = 0; std::uint32_t scalar = 0xffffffff;
    Require(!dt::DecodeCanonicalUtf8Scalar(reinterpret_cast<const std::uint8_t*>(bytes.data()),
        bytes.size(), &offset, &scalar) && offset == 0 && scalar == 0xffffffff,
        "failed scalar decode changed outputs");
    invalid_operations(bytes); invalid_operations(std::string("prefix\0",7) + bytes);
  }
  for (std::uint32_t cp : {0x80u,0x7ffu,0x800u,0xd7ffu,0xe000u,0xffffu,0x10000u,0x10ffffu}) {
    const auto valid = Utf8Scalar(cp);
    for (std::size_t length = 1; length < valid.size(); ++length) invalid_operations(valid.substr(0,length));
    for (std::size_t offset = 1; offset < valid.size(); ++offset) {
      for (unsigned byte : {0u,0x7fu,0xc0u,0xffu}) {
        auto invalid = valid; invalid[offset] = static_cast<char>(byte); invalid_operations(invalid);
      }
    }
  }
  for (const auto& bytes : {std::string{},std::string("a\0b",3),Utf8Scalar(0xffff),Utf8Scalar(0x10ffff)}) {
    dt::DatatypeOperationValue value{dt::CanonicalTypeId::character, bytes, false};
    dt::DatatypeSerializationRequest serialize; serialize.value = value;
    const auto encoded = dt::SerializeDatatypeValue(serialize); Require(encoded.ok(), "valid TEXT serialization refused");
    dt::DatatypeDeserializationRequest deserialize; deserialize.serialized_value = encoded.serialized_value;
    const auto decoded = dt::DeserializeDatatypeValue(deserialize);
    Require(decoded.ok() && decoded.value.encoded_value == bytes, "valid TEXT round trip failed");
    Compare(bytes, bytes, Seed(), 0);
  }
  std::size_t offset = 1; std::uint32_t scalar = 99;
  for (const auto* wire : {"SBDV1;type=text;state=value", "SBDV1;type=text;state=value;payload=0g"}) {
    dt::DatatypeDeserializationRequest deserialize; deserialize.serialized_value = wire;
    const auto decoded = dt::DeserializeDatatypeValue(deserialize);
    Require(!decoded.ok() && decoded.value.type_id == dt::CanonicalTypeId::unknown &&
            decoded.value.encoded_value.empty(), "missing or malformed payload published empty TEXT");
  }
  const std::uint8_t byte = 'a';
  Require(!dt::DecodeCanonicalUtf8Scalar(&byte,1,&offset,&scalar) && offset == 1 && scalar == 99,
          "end offset accepted");
  offset = std::numeric_limits<std::size_t>::max();
  Require(!dt::DecodeCanonicalUtf8Scalar(&byte,1,&offset,&scalar), "out of bounds offset accepted");
  Require(!dt::DecodeCanonicalUtf8Scalar(nullptr,1,&offset,&scalar), "null input accepted");
  Require(!dt::DecodeCanonicalUtf8Scalar(&byte,1,nullptr,&scalar), "null offset accepted");
  Require(!dt::DecodeCanonicalUtf8Scalar(&byte,1,&offset,nullptr), "null scalar accepted");
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--utf8") {
      TestUtf8Admission();
      std::cout << "PASS canonical UTF-8 checks=" << checks << '\n';
      return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--unicode") {
      TestUnicodeComparison();TestKeysAndHashes();
      std::cout << "PASS Unicode collation checks=" << checks << '\n';
      return EXIT_SUCCESS;
    }
    TestComparison(); TestRefusal(); TestAtomicity();
    std::cout << "PASS binary collation checks=" << checks << " allocation_faults=" << faults << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    fail_after = -1; std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
