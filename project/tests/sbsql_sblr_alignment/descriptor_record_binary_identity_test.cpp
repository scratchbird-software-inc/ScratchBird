// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_descriptor_record_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) { std::cerr << "check failed at line " << where.line() << '\n'; std::abort(); }
}
static std::string Unhex(std::string_view hex) {
  std::string out;
  auto nibble=[](char c) { return c <= '9' ? c-'0' : c-'a'+10; };
  for (std::size_t n=0;n<hex.size();n+=2) out.push_back(static_cast<char>((nibble(hex[n])<<4)|nibble(hex[n+1])));
  return out;
}
static auto Bytes(const std::string& s) {
  return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()),s.size());
}
int main() {
  a::EngineUuid id;
  id.bytes={1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,1};
  const a::MgaDescriptorFields fields{{std::string("key\0|",5),"value\n\t="},{"",""}};
  // Golden bytes independently packed with Python struct, including embedded
  // delimiters/NUL in fields; UUID octets occupy exactly offsets 18..33.
  const auto golden=Unhex("53424d4741444553433231000000000000000190000000007000800000000000000102000000050000006b6579007c0800000076616c75650a093d0000000000000000");
  const auto legacy=Unhex("53424d474144455343310952454c4154494f4e0930313930303030302d303030302d373030302d383030302d30303030303030303030303109366236353739303037633d373636313663373536353061303933647c3d0a");
  std::string encoded;
  Check(a::AppendMgaDescriptorRecord(id,fields,&encoded));
  Check(encoded==golden);
  const a::MgaDescriptorRecords expected{{id,fields}};
  for (const auto& data : {golden,golden+golden}) {
    a::MgaDescriptorRecords decoded;
    Check(a::DecodeMgaDescriptorRecords(Bytes(data),&decoded));
    Check(decoded==expected);
  }
  for (const auto& data : {golden,legacy}) {
    for (std::size_t n=1;n<data.size();++n) {
      auto decoded=expected;
      Check(!a::DecodeMgaDescriptorRecords(Bytes(data.substr(0,n)),&decoded));
      Check(decoded==expected);
    }
  }
  // No identity truncation: all bits are either retained or refused by the
  // engine UUID version/variant admission rules.
  for (std::size_t bit=0;bit<128;++bit) {
    auto changed=id;changed.bytes[bit/8]^=1u<<(bit%8);
    std::string data;
    const bool valid=scratchbird::core::uuid::IsEngineIdentityUuid(changed);
    Check(a::AppendMgaDescriptorRecord(changed,fields,&data)==valid);
    if (valid) {
      a::MgaDescriptorRecords decoded;
      Check(a::DecodeMgaDescriptorRecords(Bytes(data),&decoded));
      Check(decoded.contains(changed)&&!decoded.contains(id));
      Check(decoded.at(changed)==fields);
    } else {
      Check(data.empty());
      data=golden;data[18+bit/8]^=1u<<(bit%8);
      auto decoded=expected;
      Check(!a::DecodeMgaDescriptorRecords(Bytes(data),&decoded)&&decoded==expected);
    }
  }
  auto reject=[&](const std::string& data) {
    auto decoded=expected;
    Check(!a::DecodeMgaDescriptorRecords(Bytes(data),&decoded)&&decoded==expected);
  };
  for (const std::size_t offset : {std::size_t(9),std::size_t(10),std::size_t(34),std::size_t(38)}) {
    auto data=golden;data[offset]=static_cast<char>(0xff);reject(data);
  }
  auto nil=golden;nil.replace(18,16,16,'\0');reject(nil);
  reject(legacy);reject(golden+legacy);reject(legacy+golden);
  reject(golden+"x");reject(legacy+"x");reject(golden+legacy.substr(0,legacy.size()-1));
  reject("SBMGADESC1\tRELATION\ttable-label\t61=62\n");
  reject("SBMGADESC1\tRELATION\t01900000-0000-7000-8000-000000000001\t61=gg\n");
  reject("SBMGADESC1\tRELATION\t01900000-0000-7000-8000-000000000001\t61=62|\n");
  std::string later;
  const a::MgaDescriptorFields replacement{{"new","last append wins"}};
  Check(a::AppendMgaDescriptorRecord(id,replacement,&later));
  a::MgaDescriptorRecords decoded;
  Check(a::DecodeMgaDescriptorRecords(Bytes(golden+later),&decoded));
  Check(decoded.size()==1&&decoded.at(id)==replacement);
  Check(a::DecodeMgaDescriptorRecords(Bytes(later+golden),&decoded)&&decoded==expected);
  std::string empty_fields;
  Check(a::AppendMgaDescriptorRecord(id,{},&empty_fields));
  Check(a::DecodeMgaDescriptorRecords(Bytes(empty_fields),&decoded));
  Check(decoded.size()==1&&decoded.at(id).empty());
  Check(a::DecodeMgaDescriptorRecords({},&decoded)&&decoded.empty());
  encoded="unchanged";
  Check(!a::AppendMgaDescriptorRecord({},fields,&encoded)&&encoded=="unchanged");
  Check(!a::AppendMgaDescriptorRecord(id,fields,nullptr));
  Check(!a::DecodeMgaDescriptorRecords(Bytes(golden),nullptr));
}
