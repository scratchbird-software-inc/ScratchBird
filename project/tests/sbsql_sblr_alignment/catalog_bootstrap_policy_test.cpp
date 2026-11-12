// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "bootstrap_policy_fixture.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

thread_local bool heap_denied=false;
void* operator new(std::size_t n){if(heap_denied)throw std::bad_alloc();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;namespace f=scratchbird::tests::bootstrap_policy;
namespace {
unsigned checks=0;
void Check(bool valid,const char* why){++checks;if(!valid){heap_denied=false;throw std::runtime_error(why);}}
template<class R> auto Encode(const R& r,std::span<p::byte> out) {
  if constexpr(std::is_same_v<R,c::CatalogBootstrapPolicyPack>)return c::EncodeCatalogBootstrapPolicyPackInto(r,out);
  else if constexpr(std::is_same_v<R,c::CatalogBootstrapSecurityProvider>)return c::EncodeCatalogBootstrapSecurityProviderInto(r,out);
  else return c::EncodeCatalogBootstrapPolicyProfileInto(r,out);
}
template<class R> auto Decode(std::string_view s) {
  if constexpr(std::is_same_v<R,c::CatalogBootstrapPolicyPack>)return c::DecodeCatalogBootstrapPolicyPack(s);
  else if constexpr(std::is_same_v<R,c::CatalogBootstrapSecurityProvider>)return c::DecodeCatalogBootstrapSecurityProvider(s);
  else return c::DecodeCatalogBootstrapPolicyProfile(s);
}
template<class R> void Roundtrip(const R& r) {
  const auto golden=f::EncodeGolden(r);std::array<p::byte,2048> backing;backing.fill(0x5a);
  heap_denied=true;const auto encoded=Encode(r,backing);const auto decoded=Decode<R>(golden);heap_denied=false;
  Check(encoded.ok()&&decoded.ok()&&encoded.bytes.size()==golden.size()&&
      std::equal(encoded.bytes.begin(),encoded.bytes.end(),reinterpret_cast<const p::byte*>(golden.data()))&&
      f::EncodeGolden(*decoded.record)==golden,"independent exact binary oracle/heap-denied decode and encode");
  Check(std::all_of(backing.begin()+golden.size(),backing.end(),[](auto v){return v==0x5a;}),"no writes past exact frame");
  // Every output/input truncation remains fail-atomic, including valid fields
  // preceding a malformed final field. No error path is allowed to allocate.
  for(std::size_t n=0;n<golden.size();++n) {
    backing.fill(0x5a);heap_denied=true;
    const auto refused=Encode(r,std::span<p::byte>(backing).first(n));
    const auto truncated=Decode<R>(std::string_view(golden).substr(0,n));heap_denied=false;
    Check(!refused.ok()&&refused.bytes.empty()&&!truncated.ok()&&!truncated.record&&
        std::all_of(backing.begin(),backing.end(),[](auto v){return v==0x5a;}),"all truncated input/backing refuses unchanged without allocation");
  }
  for(const auto at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}) {
    auto bad=golden;bad[at]^=1;heap_denied=true;const auto refused=Decode<R>(bad);heap_denied=false;
    Check(!refused.ok()&&!refused.record,"malformed framing/schema/field does not escape");
  }
}
template<class R> void Refused(const R& r) {
  const auto golden=f::EncodeGolden(r);
  std::array<p::byte,2048> backing;backing.fill(0x5a);heap_denied=true;
  const auto refused=Encode(r,backing);
  const auto decoded=Decode<R>(golden);heap_denied=false;
  Check(!refused.ok()&&refused.bytes.empty()&&std::all_of(backing.begin(),backing.end(),[](auto v){return v==0x5a;}),
        "malformed native definition refuses without allocation or partial output");
  if(r.origin_transaction_uuid.kind==p::UuidKind::transaction)
    Check(!decoded.ok()&&!decoded.record,"malformed independently framed family refuses borrowed decode too");
}
void Tests() {
  const p::TypedUuid tx{p::UuidKind::transaction,f::Id(5)};f::Fixture fixture(tx);
  Roundtrip(fixture.pack);Roundtrip(fixture.profile);
  for(unsigned family=1;family<=12;++family)for(bool enabled:{false,true}) {
    auto r=fixture.provider;r.family=static_cast<c::BootstrapProviderFamily>(family);r.enabled_by_default=enabled;
    if(family!=1){r.external_provider=true;r.authority=c::BootstrapProviderAuthority::external_provider_admission;r.verifier_policy.reset();}
    Roundtrip(r);
  }
  for(const auto maximum:{p::u64{1},std::numeric_limits<p::u64>::max()}) {
    auto pack=fixture.pack;pack.generation=pack.origin_local_transaction_id=pack.imported_schema_version=maximum;
    pack.local_password_only=false;pack.declared_counts.fill(maximum);pack.declared_counts[2]=0;Roundtrip(pack);
    auto provider=fixture.provider;provider.generation=provider.policy_pack_generation=provider.origin_local_transaction_id=maximum;Roundtrip(provider);
    auto profile=fixture.profile;profile.generation=profile.policy_pack_generation=profile.origin_local_transaction_id=maximum;
    profile.resource_definition_uuid=f::Id(9200);profile.resource_definition_generation=maximum;Roundtrip(profile);
  }
  for(unsigned bad=0;bad<10;++bad) {
    auto r=fixture.provider;
    if(bad==0)r.generation=0;if(bad==1)r.policy_pack_generation=0;if(bad==2)r.provider_uuid=r.policy_pack_uuid;
    if(bad==3)r.origin_local_transaction_id=0;if(bad==4)r.family=static_cast<c::BootstrapProviderFamily>(13);
    if(bad==5)r.authority=static_cast<c::BootstrapProviderAuthority>(3);if(bad==6)r.external_provider=true;
    if(bad==7)r.verifier_policy.reset();if(bad==8)r.verifier_policy=static_cast<c::BootstrapVerifierPolicy>(2);
    if(bad==9)r.origin_transaction_uuid.kind=p::UuidKind::row;Refused(r);
  }
  for(unsigned bad=0;bad<8;++bad) {
    auto r=fixture.pack;if(bad==0)r.post_create_filesystem_authority=true;if(bad==1)r.generation=0;
    if(bad==2)r.imported_schema_version=0;if(bad==3)r.declared_counts[1]=0;if(bad==4)r.declared_counts[2]=12;
    // 3b: cases 5/6 mutated the retired text id/version; the status constant
    // takes their places (zero and an unknown value refuse).
    if(bad==5)r.declared_signature_status=static_cast<c::BootstrapSignatureStatus>(0);
    if(bad==6)r.declared_signature_status=static_cast<c::BootstrapSignatureStatus>(2);
    if(bad==7)r.policy_pack_uuid=r.origin_transaction_uuid.value;Refused(r);
  }
  for(unsigned bad=0;bad<7;++bad) {
    auto r=fixture.profile;if(bad==0)r.generation=0;if(bad==1)r.policy_pack_generation=0;if(bad==2)r.mode=static_cast<c::BootstrapPolicyMode>(0);/*3b*/
    if(bad==3)r.profile_uuid=r.policy_pack_uuid;if(bad==4)r.resource_definition_uuid=p::Uuid{};
    if(bad==5)r.resource_definition_uuid=r.origin_transaction_uuid.value;
    if(bad==6)r.origin_transaction_uuid.kind=p::UuidKind::unknown;Refused(r);
  }
  for(unsigned version=0;version<16;++version)if(version!=7) {
    auto pack=fixture.pack;pack.policy_pack_uuid.bytes[6]=version<<4;Refused(pack);
    auto provider=fixture.provider;provider.policy_pack_uuid.bytes[6]=version<<4;Refused(provider);
    auto profile=fixture.profile;profile.profile_uuid.bytes[6]=version<<4;Refused(profile);
  }
  for(unsigned bad=0;bad<4;++bad) {
    auto profile=fixture.profile;
    if(bad==0)profile.resource_definition_uuid=f::Id(9200);
    if(bad==1)profile.resource_definition_generation=0;
    if(bad==2)profile.resource_definition_generation=1;
    if(bad==3){profile.resource_definition_uuid=f::Id(9200);profile.resource_definition_generation=0;}
    Refused(profile);
  }
  // 3b: every area and mode constant roundtrips; zero and one past the last
  // refuse. The pack's spellings map to exactly these values, unknown refuses.
  for(p::u64 area=1;area<=c::kBootstrapPolicyAreaCount;++area)for(p::u64 mode=1;mode<=c::kBootstrapPolicyModeCount;++mode){
    auto r=fixture.profile;r.area=static_cast<c::BootstrapPolicyArea>(area);r.mode=static_cast<c::BootstrapPolicyMode>(mode);Roundtrip(r);}
  for(const p::u64 bad:{p::u64{0},p::u64{15}}){
    auto r=fixture.profile;r.area=static_cast<c::BootstrapPolicyArea>(bad);Refused(r);
    r=fixture.profile;r.mode=static_cast<c::BootstrapPolicyMode>(bad);Refused(r);}
  {
    constexpr std::string_view areas[]{"security_provider_selection","standard_roles_groups_grants","default_security_posture",
      "memory_resource_governance","storage_filespace_page_policy","transaction_mga_cleanup_archive_backup_forward",
      "optimizer_statistics_feedback","index_maintenance","agent_policy","diagnostics","observability",
      "unsupported_feature_behavior","cluster_boundary","release_default_configuration"};
    constexpr std::string_view modes[]{"local_password_only","uuid_catalog_seed","deny_by_default","configured_policy_required",
      "local_durable_fail_closed","mga_inventory_authority","catalog_backed_or_diagnostic_only","provider_admission_required",
      "evidence_not_authority","stable_redacted_diagnostics","redacted_evidence_only","deterministic_fail_closed",
      "external_provider_required","secure_defaults"};
    for(p::u64 n=0;n<14;++n){
      Check(c::BootstrapPolicyAreaFromImportText(areas[n])==static_cast<c::BootstrapPolicyArea>(n+1),"3b: pack area text maps to its frozen constant");
      Check(c::BootstrapPolicyModeFromImportText(modes[n])==static_cast<c::BootstrapPolicyMode>(n+1),"3b: pack mode text maps to its frozen constant");}
    for(const std::string_view unknown:{"","Security_Provider_Selection","local-password-only","updated-mode","security_provider_selection "}){
      Check(!c::BootstrapPolicyAreaFromImportText(unknown)&&!c::BootstrapPolicyModeFromImportText(unknown),"3b: unknown area/mode text refuses, nothing defaults");}
    Check(c::BootstrapSignatureStatusFromImportText("signature-ready-unsigned")==c::BootstrapSignatureStatus::signature_ready_unsigned&&
        static_cast<p::u64>(c::BootstrapSignatureStatus::signature_ready_unsigned)==1&&
        !c::BootstrapSignatureStatusFromImportText("signed")&&!c::BootstrapSignatureStatusFromImportText("signature_ready_unsigned"),
        "3b: the signature status maps exactly; unknown refuses");
  }
  {
    // 3b: the retired version-1 layouts (pack text id/version/status, profile
    // area/mode text), independently framed, refuse; both are still recognized
    // as this family so they refuse rather than pass as foreign payloads.
    const auto& r=fixture.pack;f::Golden pack(65588,1);pack.Identity(1,r.policy_pack_uuid);pack.Integer(2,r.generation);
    pack.Field(3,3,"selected-pack");pack.Field(4,3,"1.0.0");
    pack.Field(5,4,{reinterpret_cast<const char*>(r.content_sha256.data()),32});pack.Field(6,3,"signature-ready-unsigned");
    pack.Boolean(7,r.create_time_only);pack.Boolean(8,r.post_create_filesystem_authority);pack.Boolean(9,r.local_password_only);
    pack.Identity(10,r.origin_transaction_uuid.value);pack.Integer(11,r.origin_local_transaction_id);
    for(unsigned i=0;i<9;++i)pack.Integer(12+i,r.declared_counts[i]);pack.Integer(21,r.imported_schema_version);
    const auto& q=fixture.profile;f::Golden profile(65590,1);profile.Identity(1,q.profile_uuid);profile.Integer(2,q.generation);
    profile.Identity(3,q.policy_pack_uuid);profile.Integer(4,q.policy_pack_generation);
    profile.Field(5,3,"security_provider_selection");profile.Field(6,3,"local_password_only");
    profile.Identity(8,q.origin_transaction_uuid.value);profile.Integer(9,q.origin_local_transaction_id);
    const auto old_pack=pack.Finish(),old_profile=profile.Finish();
    heap_denied=true;const auto a=c::DecodeCatalogBootstrapPolicyPack(old_pack);const auto b=c::DecodeCatalogBootstrapPolicyProfile(old_profile);heap_denied=false;
    Check(!a.ok()&&!a.record&&!b.ok()&&!b.record&&c::IsCatalogBootstrapPolicyPayload(old_pack)&&c::IsCatalogBootstrapPolicyPayload(old_profile),
        "3b: version-1 pack and profile refuse");
    for(unsigned i:{0u,2u}){auto m=f::Metadata(i,tx,f::Id(60),i?old_profile:old_pack);
      Check(!c::CatalogBootstrapPolicyMatchesMetadata(c::BorrowCatalogMetadataVersion(m))&&
          !c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)),"3b: the common dispatcher refuses version 1 too");}
  }
  const auto payloads=fixture.Payloads();
  auto replacement=fixture;replacement.pack.generation=replacement.provider.generation=replacement.profile.generation=2;
  // 3b: the generation-2 changes are to fields that still exist (was pack_version
  // text and a free-text mode).
  replacement.pack.content_sha256[0]^=0xff;replacement.provider.enabled_by_default=false;
  replacement.profile.mode=c::BootstrapPolicyMode::secure_defaults;
  const auto replacement_payloads=replacement.Payloads();
  for(unsigned i=0;i<3;++i) {
    auto m=f::Metadata(i,tx,f::Id(60),payloads[i]);heap_denied=true;
    const auto valid=c::CatalogBootstrapPolicyMatchesMetadata(c::BorrowCatalogMetadataVersion(m));
    const auto size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m));heap_denied=false;
    Check(valid&&size,"native common dispatcher validates complete family with no allocation");
    const auto encoded=c::EncodeCatalogMetadataVersion(m);Check(encoded.ok(),"full native common envelope admits family");
    const auto reopened=c::DecodeCatalogMetadataVersion(encoded.bytes);Check(reopened.ok()&&reopened.record.record.payload==payloads[i],"complete common byte roundtrip");
    for(unsigned bad=0;bad<8;++bad) {
      auto altered=m;if(bad==0)altered.record.header.object_uuid.value=f::Id(9999);
      if(bad==1)altered.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;if(bad==2)altered.default_name_uuid={};
      if(bad==3)altered.name_vector_uuid={};if(bad==4)altered.owning_schema_uuid={};
      if(bad==5)altered.record.header.parent_uuid.value=f::Id(9999);if(bad==6)altered.definition_version=2;
      if(bad==7)altered.record.header.kind=c::CatalogRecordKind::config_profile;
      heap_denied=true;const auto refused=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(altered));heap_denied=false;
      Check(!refused,"family/header/subtype/name/parent/version confusion refuses in shared native dispatcher");
    }
    auto next=m;next.creator_transaction_uuid.value=f::Id(6);next.creator_local_transaction_id=2;
    heap_denied=true;const auto same=c::CatalogMetadataPreservesFamilyOrigin(m,next);heap_denied=false;
    Check(!same,"first-version origin cannot be rewritten through common family dispatcher");
    next.definition_version=2;next.record.payload=replacement_payloads[i];
    heap_denied=true;const auto successor=c::CatalogMetadataPreservesFamilyOrigin(m,next);
    const auto successor_size=c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(next));heap_denied=false;
    Check(successor&&successor_size,"actual generation2 fields can change while immutable family/package/origin persist");
    auto altered_origin=replacement;
    altered_origin.pack.origin_transaction_uuid.value=altered_origin.provider.origin_transaction_uuid.value=
        altered_origin.profile.origin_transaction_uuid.value=f::Id(6);
    next.record.payload=altered_origin.Payloads()[i];
    heap_denied=true;const auto origin_valid=c::CatalogBootstrapPolicyMatchesMetadata(c::BorrowCatalogMetadataVersion(next));
    const auto rewritten=c::CatalogMetadataPreservesFamilyOrigin(m,next);heap_denied=false;
    Check(origin_valid&&!rewritten,"independently valid generation2 cannot rewrite immutable origin");
  }
}
} // namespace
int main()try{Tests();std::cout<<"PASS native bootstrap policy definitions "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){heap_denied=false;std::cerr<<e.what()<<'\n';return 1;}
