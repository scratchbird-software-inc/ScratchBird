// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/storage/filespace/filespace_package.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace fs = scratchbird::storage::filespace;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(UuidKind kind,unsigned n) {
  auto id=scratchbird::tests::FixtureUuid(1205,n);id.bytes[10]=0;id.bytes[9]=255;
  const auto bound=uuid::MakeTypedUuid(kind,id);Check(bound.ok());return bound.value;
}
int main() {
  fs::FilespacePackageManifest manifest;
  manifest.package_uuid=Id(UuidKind::object,1);manifest.source_database_uuid=Id(UuidKind::database,2);
  manifest.package_name="package|with\nseparators";
  fs::FilespacePackageMember member;
  member.database_uuid=manifest.source_database_uuid;member.filespace_uuid=Id(UuidKind::filespace,3);
  member.writer_identity_uuid=Id(UuidKind::object,4);member.path="/fixture/member|with\nseparator";
  member.role=fs::FilespaceRole::secondary_data;member.state=fs::FilespaceState::detached;
  member.page_size=16384;member.physical_filespace_id=7;member.header_generation=1;
  member.member_checksum=fs::DigestString(fs::MemberPayload(member));manifest.members.push_back(member);
  manifest.manifest_checksum=fs::DigestString(fs::ManifestPayload(manifest));
  const auto bytes=fs::SerializePackageManifestFile(manifest);
  const auto read=fs::ParsePackageManifestFileContent(bytes,bytes.size());
  Check(read.ok() && read.checksum_verified && read.manifest.format_version==2);
  Check(read.manifest.package_uuid.value==manifest.package_uuid.value);
  Check(read.manifest.members[0].filespace_uuid.value==member.filespace_uuid.value);
  Check(read.manifest.members[0].writer_identity_uuid.value==member.writer_identity_uuid.value);
  Check(read.manifest.members[0].path==member.path && read.manifest.package_name==manifest.package_name);
  Check(fs::SerializePackageManifestFile(read.manifest)==bytes);
  for(std::size_t n=0;n<bytes.size();++n) Check(!fs::ParsePackageManifestFileContent(bytes.substr(0,n),n).ok());
  Check(!fs::ParsePackageManifestFileContent(bytes+"x",bytes.size()+1).ok());
  Check(!fs::ParsePackageManifestFileContent("SBFS_PACKAGE_MANIFEST_V1\n",24).ok());
  auto fields=fs::DecodePackageFields(bytes);fields[1]="package_uuid=019f0000-0000-7000-8000-000000000001";
  auto invalid=fs::EncodePackageFields(fields);Check(!fs::ParsePackageManifestFileContent(invalid,invalid.size()).ok());
  fields=fs::DecodePackageFields(bytes);fields[5]="root_authority_present=2";
  invalid=fs::EncodePackageFields(fields);Check(!fs::ParsePackageManifestFileContent(invalid,invalid.size()).ok());
  auto changed=manifest;changed.members[0].filespace_uuid=Id(UuidKind::filespace,5);
  invalid=fs::SerializePackageManifestFile(changed);Check(!fs::ParsePackageManifestFileContent(invalid,invalid.size()).ok());
}
