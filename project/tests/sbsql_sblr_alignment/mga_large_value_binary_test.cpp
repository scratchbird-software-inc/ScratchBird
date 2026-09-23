// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/mga_relation_store/mga_large_value_store.cpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
a::EngineUuid Id(unsigned value) {
  return a::EngineUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(value)}};
}
int main() {
  const auto id=Id(58); // includes delimiter-looking octets, including NUL
  const auto locator=a::MakeMgaLargeValueLocator(id,123,456);
  Check(locator.size()==40 && locator.substr(8,16)==a::MetadataUuidBytes(id));
  a::EngineUuid decoded;
  std::uint64_t checksum=0,size=0;
  Check(a::ReadMgaLargeValueLocator(locator,&decoded,&checksum,&size));
  Check(decoded==id && checksum==123 && size==456);
  for(std::size_t n=0;n<locator.size();++n) {
    decoded=Id(99);checksum=77;size=88;
    Check(!a::ReadMgaLargeValueLocator(locator.substr(0,n),&decoded,&checksum,&size));
    Check(decoded==Id(99)&&checksum==77&&size==88);
  }
  Check(!a::ReadMgaLargeValueLocator("SBMGA_LARGE_VALUE:01900000-0000-7000-8000-00000000003a:123:456",&decoded,&checksum,&size));
  const std::string payload("a\0b\t\n:c",8);
  const std::vector<std::string> header={"SBMGL002","LARGE_VALUE","7",a::MetadataUuidBytes(id),a::MetadataUuidBytes(Id(2)),a::MetadataUuidBytes(Id(3)),a::MetadataUuidBytes(Id(4)),"field",std::to_string(payload.size()),std::to_string(a::ChecksumText(payload)),"durable_uncommitted"};
  const std::vector<std::string> chunk={"SBMGL002","LARGE_VALUE_CHUNK","7",a::MetadataUuidBytes(id),"0",payload,std::to_string(a::ChecksumText(payload))};
  const auto frame=a::JoinLine(header), fragment=a::JoinLine(chunk);
  Check(!frame.empty()&&!fragment.empty());
  Check(a::SplitTabs(frame)==header&&a::SplitTabs(fragment)==chunk);
  auto invalid=header;invalid[3]="01900000-0000-7000-8000-00000000003a";
  Check(a::JoinLine(invalid).empty());
  invalid=header;invalid[4].resize(15);Check(a::JoinLine(invalid).empty());
  invalid=header;invalid[2]="0";Check(a::JoinLine(invalid).empty());
  for(std::size_t n=0;n<frame.size();++n)Check(a::SplitTabs(frame.substr(0,n)).empty());
  auto corrupt=fragment;corrupt[30]^=1;Check(a::SplitTabs(corrupt).empty());
  auto path=std::filesystem::temp_directory_path()/("sb-large-binary-"+std::to_string(getpid()));
  std::uint64_t opens=0,flushes=0;
  Check(a::AppendLines(path.string(),{frame,fragment},&opens,&flushes)&&opens==1&&flushes==1);
  std::ifstream input(path,std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)),{});
  Check(bytes==frame+fragment);
  std::vector<std::string> records;
  Check(a::DecodeMgaMetadataStream({reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()},&records)&&records==std::vector<std::string>({frame,fragment}));
  std::filesystem::remove(path);
}
