// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/security/security_principal_lifecycle.cpp"
#include "../../src/storage/database/database_local_private_relation_locator.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace codec = db::security_event_codec;
using scratchbird::tests::FixtureUuid;
using scratchbird::tests::FixtureUuidLiteral;
void Check(bool value) { if (!value) std::abort(); }
int main() {
  auto uuid = FixtureUuid(1302, 1);
  uuid.bytes[9]=0; uuid.bytes[10]='\n'; uuid.bytes[11]='\t'; uuid.bytes[12]=255;
  api::EngineSecurityPrivilegeGrantRecord grant;
  grant.creator_tx=7;grant.grant_uuid=uuid;grant.grantee_uuid=FixtureUuid(1302,2);
  grant.target_object_uuid=FixtureUuid(1302,3);grant.grantor_principal_uuid=FixtureUuid(1302,4);
  grant.grantee_kind="principal";grant.target_object_kind="table";grant.privilege="SELECT";
  grant.grant_effect="allow";grant.security_generation=2;
  const auto event=api::GrantEvent(grant);
  const auto decoded=codec::Decode(event);
  Check(decoded.size()==13 && decoded.identity(3)==uuid && decoded.identity(4)==grant.grantee_uuid);
  Check(decoded[3].empty()); // UUID bytes are never exposed through text projection.
  api::EngineSecurityAuditRecord audit;
  audit.creator_tx=7;audit.audit_uuid=FixtureUuid(1302,5);
  audit.operation_id="security.privilege.grant";audit.actor_principal_uuid=grant.grantor_principal_uuid;
  audit.target_uuid=grant.target_object_uuid;audit.outcome="success";audit.security_generation=2;
  audit.related_identity_uuid=grant.grantee_uuid;audit.related_identity_role="grantee_uuid";
  api::EngineRequestContext context;context.local_transaction_id=7;
  db::DatabaseLocalSecurityBatchEnvelopeV1 batch;
  batch.database_uuid=FixtureUuid(1302,6);batch.relation_uuid=FixtureUuidLiteral("018f7a10-1280-7000-8000-000000000107");
  batch.transaction_uuid=FixtureUuid(1302,7);batch.actor_principal_uuid=grant.grantor_principal_uuid;
  batch.creator_local_transaction_id=7;batch.prior_security_context_generation=1;batch.successor_security_context_generation=2;
  batch.events={event,api::AuditEvent(audit),api::CacheInvalidationEvent(context,"grant",grant.target_object_uuid,2)};
  const auto payload=codec::Frame(batch.events);
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size());
  Check(digest.ok());
  batch.events.push_back(codec::Encode("AUTH_CONTEXT_SUCCESSOR",7,{std::string("2"),std::string("security-context-successor:v1:sha256:")+scratchbird::core::hash::HexLower(digest.digest)}));
  std::string refusal;
  Check(db::ValidateDatabaseLocalSecurityLifecycleBatchV1(batch,&refusal));
  auto wrong=batch;wrong.actor_principal_uuid=FixtureUuid(1302,8);
  Check(!db::ValidateDatabaseLocalSecurityLifecycleBatchV1(wrong,&refusal));
  auto bytes=db::EncodeDatabaseLocalSecurityBatchEnvelopeV1(batch);
  Check(!bytes.empty());
  db::DatabaseLocalSecurityBatchEnvelopeV1 roundtrip;
  Check(db::DecodeDatabaseLocalSecurityBatchEnvelopeV1(bytes,&roundtrip,&refusal));
  Check(roundtrip.events==batch.events);
  for(std::size_t size=0;size<event.size();++size) Check(codec::Decode(std::string_view(event).substr(0,size)).size()==0);
  Check(codec::Decode(event+"x").size()==0);
  Check(codec::Decode("SBSECPL1\tGRANT\t7").size()==0);
  auto mixed=decoded;
  mixed.fields[3]=std::string("019f0000-0000-7000-8000-000000000001");
  Check(!codec::Valid(mixed));
  api::EngineSecurityPrivilegeTemplateRecord templ;
  templ.creator_tx=7;templ.template_uuid=uuid;templ.grantee_uuids={uuid,grant.grantee_uuid};
  const auto template_event=api::PrivilegeTemplateEvent(templ);
  Check(codec::Decode(template_event).identities(8)==templ.grantee_uuids);
  const auto framed=codec::Frame(batch.events);
  std::vector<std::string> restored;
  Check(codec::Unframe(framed,&restored) && restored==batch.events);
  for(std::size_t size=1;size<event.size()+4;++size) {
    std::vector<std::string> unchanged={"sentinel"};
    Check(!codec::Unframe(std::string_view(framed).substr(0,size),&unchanged));
    Check(unchanged==std::vector<std::string>{"sentinel"});
  }
  std::cout<<"security native event/storage batch regression PASS\n";
}
