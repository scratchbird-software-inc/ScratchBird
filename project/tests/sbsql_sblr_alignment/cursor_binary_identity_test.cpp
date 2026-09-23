// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_cursor_open_coordinator.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool v, std::source_location at=std::source_location::current()) {
  if (!v) { std::cerr << "check failed at " << at.line() << '\n'; std::abort(); }
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid id;id.bytes={1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;
}
static std::string Evidence(const std::string& s) {
  auto h=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(s.begin(),s.end()));
  Check(h.ok());return "sha256:"+scratchbird::core::hash::HexLower(h.digest);
}
int main() {
  auto pattern=(std::filesystem::temp_directory_path()/"sb-cursor-binary-XXXXXX").string();
  auto* path=mkdtemp(pattern.data());Check(path);
  struct Cleanup { std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);} } cleanup{path};
  a::EngineRequestContext c;
  c.database_path=(cleanup.path/"database").string();c.database_uuid=Id(1);
  c.session_uuid=Id(2);c.principal_uuid=Id(3);c.statement_uuid=Id(4);
  c.statement_metadata_snapshot_engine_owned=true;c.security_context_present=true;
  c.trace_tags={"private_executable_plan_receipt_compiler","private_cursor_open","private_cursor_fetch","private_cursor_close","right:SBLR_CURSOR_ADMIN"};
  const auto compiled=a::CompileAndPublishSblrExecutablePlanReceipt(c,c.statement_uuid,1,1,1,8,1);
  Check(compiled.ok);const auto& p=compiled.snapshot;
  Check(p.receipt_uuid==c.statement_uuid&&p.session_uuid==c.session_uuid&&p.security_uuid==c.principal_uuid);
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(p.descriptor_uuid));
  for(unsigned bit=0;bit<128;++bit) {
    auto wrong=c;wrong.database_uuid.bytes[bit/8]^=1u<<(bit%8);
    Check(!a::OpenSblrCursor(wrong,p.descriptor_uuid,p.descriptor_generation,p.descriptor_evidence_sha256,1).ok);
    wrong=c;wrong.session_uuid.bytes[bit/8]^=1u<<(bit%8);
    Check(!a::OpenSblrCursor(wrong,p.descriptor_uuid,p.descriptor_generation,p.descriptor_evidence_sha256,1).ok);
  }
  const auto opened=a::OpenSblrCursor(c,p.descriptor_uuid,p.descriptor_generation,p.descriptor_evidence_sha256,1);
  Check(opened.ok);const auto& o=opened.snapshot;
  for(unsigned bit=0;bit<128;++bit) {
    auto wrong=o.cursor_uuid;wrong.bytes[bit/8]^=1u<<(bit%8);
    Check(!a::FetchSblrCursor(c,wrong,o.cursor_generation,o.position_generation,Evidence(o.cursor_evidence_sha256),1,8).ok);
  }
  const auto fetched=a::FetchSblrCursor(c,o.cursor_uuid,o.cursor_generation,o.position_generation,Evidence(o.cursor_evidence_sha256),1,8);
  Check(fetched.ok&&fetched.snapshot.position_generation==2);
  const auto& f=fetched.snapshot;
  Check(a::CloseSblrCursor(c,f.cursor_uuid,f.cursor_generation,f.position_generation,Evidence(f.cursor_evidence_sha256),1,1).ok);
  Check(!a::FetchSblrCursor(c,f.cursor_uuid,f.cursor_generation,f.position_generation,Evidence(f.cursor_evidence_sha256),1,8).ok);
  Check(!std::filesystem::exists(c.database_path+".sb.sblr_cursor_open.v1"));
  std::ifstream file(c.database_path+".sb.sblr_cursor_open.v2",std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)),{});
  Check(bytes.size()==49*4);
  const std::string events="POFC";
  for(std::size_t n=0;n<4;++n) {
    auto start=n*49;Check(bytes.substr(start,8)=="SBCUROP2"&&bytes[start+8]==events[n]);
    for(std::size_t j=0;j<16;++j) {
      Check(static_cast<std::uint8_t>(bytes[start+9+j])==p.descriptor_uuid.bytes[j]);
      Check(static_cast<std::uint8_t>(bytes[start+25+j])==(n?o.cursor_uuid.bytes[j]:0));
    }
    std::uint64_t generation=0;
    for(unsigned j=0;j<8;++j)generation|=std::uint64_t(static_cast<std::uint8_t>(bytes[start+41+j]))<<(8*j);
    Check(generation==(n?o.cursor_generation:0));
  }
}
