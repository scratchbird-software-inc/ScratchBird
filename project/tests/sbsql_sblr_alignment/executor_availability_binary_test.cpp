// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/sblr_executor_availability_registry.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value, std::source_location where=std::source_location::current()) {
  if (!value) {std::cerr << "failed line " << where.line() << '\n'; std::abort();}
}
int main() {
  a::DecodedPair pair;
  pair.snapshot.database_uuid=scratchbird::tests::FixtureUuid(1045,1);
  pair.snapshot.snapshot_uuid=scratchbird::tests::FixtureUuid(1045,2);
  pair.snapshot.generation=1;pair.snapshot.installed=true;
  pair.snapshot.availability_state=a::SblrExecutorAvailabilityState::installed;
  pair.snapshot.row_identity_sha256=a::Sha256("row");pair.reason_code="test.bootstrap";
  pair.snapshot.decision_evidence_sha256=a::Sha256(a::DecisionPayload(
      pair.snapshot.database_uuid,{},0,pair.snapshot,pair.reason_code));
  const auto frame=a::JoinRecord("EVIDENCE",pair.snapshot,{},0,pair.reason_code);
  a::DecodedPair decoded;
  Check(a::DecodeRecord(frame,"EVIDENCE",&decoded));
  Check(decoded.snapshot.database_uuid==pair.snapshot.database_uuid);
  Check(decoded.snapshot.snapshot_uuid==pair.snapshot.snapshot_uuid);
  std::vector<std::string> fields;Check(a::DecodeMgaMetadataFields(frame,&fields));
  Check(fields[3]==a::MetadataUuidBytes(pair.snapshot.database_uuid) && fields[3].size()==16);
  const auto original=fields;
  fields[3]="019f0000-0000-7000-8000-000000000001";
  Check(!a::DecodeRecord(a::EncodeMgaMetadataFields(fields),"EVIDENCE",&decoded));
  fields=original;fields[5]="00";
  Check(!a::DecodeRecord(a::EncodeMgaMetadataFields(fields),"EVIDENCE",&decoded));
  for(std::size_t n=0;n<frame.size();++n) {
    auto damaged=frame;damaged[n]^=1;
    Check(!a::DecodeRecord(damaged,"EVIDENCE",&decoded));
    Check(!a::DecodeRecord(std::string_view(frame).substr(0,n),"EVIDENCE",&decoded));
  }
  Check(decoded.snapshot.snapshot_uuid==pair.snapshot.snapshot_uuid);
  const auto ordinal=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory=std::filesystem::temp_directory_path()/("sb_availability_binary_"+std::to_string(ordinal));
  Check(std::filesystem::create_directory(directory));
  a::EngineRequestContext context;context.database_uuid=pair.snapshot.database_uuid;
  context.database_path=(directory/"database.sbdb").string();
  a::SblrExecutorAvailabilityRowIdentity identity;
  const auto loaded=a::LoadSblrExecutorAvailabilitySnapshot(context,identity);
  Check(loaded.ok && loaded.snapshot.generation==1 && loaded.snapshot.installed);
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(loaded.snapshot.snapshot_uuid));
  a::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid=context.database_uuid;request.exact_row_identity=identity;
  request.expected_snapshot_uuid=loaded.snapshot.snapshot_uuid;request.expected_generation=1;
  request.requested_state=a::SblrExecutorAvailabilityState::revoked;request.reason_code="test.revoke";
  Check(!a::SetSblrExecutorAvailability(context,request).ok);
  context.security_context_present=true;context.trace_tags={"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  const auto changed=a::SetSblrExecutorAvailability(context,request);
  Check(changed.ok && changed.snapshot.generation==2 && !changed.snapshot.installed);
  Check(!a::SetSblrExecutorAvailability(context,request).ok);
  a::VerifiedSnapshotCache().clear();
  const auto current=a::LoadCurrentSblrExecutorAvailabilitySnapshot(context,identity);
  Check(current.ok && current.snapshot.snapshot_uuid==changed.snapshot.snapshot_uuid);
  const auto path=a::StorePath(context,identity);
  const auto size=std::filesystem::file_size(path);
  std::filesystem::resize_file(path,size-1);a::VerifiedSnapshotCache().clear();
  Check(!a::LoadCurrentSblrExecutorAvailabilitySnapshot(context,identity).ok);
  {std::ofstream old(path,std::ios::binary|std::ios::trunc);old << "SBEXAV1\tEVIDENCE\tlegacy\n";}
  a::VerifiedSnapshotCache().clear();
  Check(!a::LoadCurrentSblrExecutorAvailabilitySnapshot(context,identity).ok);
  std::filesystem::remove_all(directory);
}
