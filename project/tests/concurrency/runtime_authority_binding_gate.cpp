// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_runtime_authority_binding.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace c = scratchbird::core::catalog;
using namespace scratchbird::core::platform;
using Binding = c::CatalogRuntimeAuthorityBinding;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
Uuid Id(byte tag) { return {{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,tag}}; }
void Put(std::string& bytes, std::size_t offset, u64 value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) { bytes.at(offset+i) = static_cast<char>(value & 255); value >>= 8; }
}
void Field(std::string& bytes, u16 id, byte type, std::string value) {
  const auto offset = bytes.size(); bytes.resize(offset+8, 0);
  Put(bytes, offset, id, 2); Put(bytes, offset+2, type, 1); Put(bytes, offset+4, value.size(), 4);
  bytes += value;
}
std::string Number(u64 n) { std::string bytes(8, 0); Put(bytes, 0, n, 8); return bytes; }
std::string Identity(Uuid id) { return {reinterpret_cast<const char*>(id.bytes.data()), 16}; }
// Independent wire oracle: no production schema or encoder used here.
std::string Oracle(const Binding& r) {
  std::string bytes(24, 0); bytes.replace(0, 4, "SBCV"); Put(bytes, 4, 1, 2); Put(bytes, 6, 24, 2);
  Put(bytes, 12, r.credential_reference_uuid ? 15 : 14, 4); Put(bytes, 16, 65587, 4); Put(bytes, 20, 1, 2);
  Field(bytes, 1, 5, Identity(r.binding_uuid)); Field(bytes, 2, 1, Number(r.generation));
  Field(bytes, 3, 5, Identity(r.database_uuid)); Field(bytes, 4, 5, Identity(r.service_principal_uuid));
  Field(bytes, 5, 5, Identity(r.security_authority_uuid)); Field(bytes, 6, 5, Identity(r.provider_uuid));
  Field(bytes, 7, 5, Identity(r.policy_uuid));
  if (r.credential_reference_uuid) Field(bytes, 8, 5, Identity(*r.credential_reference_uuid));
  Field(bytes, 9, 1, Number(static_cast<u64>(r.authority_mode)));
  Field(bytes, 10, 1, Number(r.security_epoch)); Field(bytes, 11, 1, Number(r.policy_epoch));
  Field(bytes, 12, 1, Number(r.provider_generation)); Field(bytes, 13, 1, Number(r.catalog_generation));
  Field(bytes, 14, 5, Identity(r.origin_transaction_uuid.value)); Field(bytes, 15, 1, Number(r.origin_local_transaction_id));
  Put(bytes, 8, bytes.size(), 4); return bytes;
}
Binding Example() {
  Binding r; r.binding_uuid=Id(1); r.database_uuid=Id(2); r.service_principal_uuid=Id(3);
  r.security_authority_uuid=r.database_uuid; r.provider_uuid=Id(4); r.policy_uuid=Id(5);
  r.generation=1; r.authority_mode=c::RuntimeAuthorityMode::database_local;
  r.security_epoch=7; r.policy_epoch=9; r.provider_generation=11; r.catalog_generation=13;
  r.origin_transaction_uuid={UuidKind::transaction,Id(6)}; r.origin_local_transaction_id=17; return r;
}
c::CatalogMetadataVersion Metadata(const Binding& r) {
  c::CatalogMetadataVersion m;
  m.record.header={c::CatalogRecordKind::config_profile,{UuidKind::row,Id(7)},
      {UuidKind::object,r.binding_uuid},{UuidKind::object,Id(8)},1,false};
  m.record.payload=Oracle(r); m.owning_schema_uuid={UuidKind::schema,Id(8)};
  m.owner_uuid={UuidKind::principal,Id(9)}; m.audit_uuid={UuidKind::object,Id(10)};
  m.security_policy_uuid={UuidKind::object,r.policy_uuid};
  m.creator_transaction_uuid=r.origin_transaction_uuid; m.creator_local_transaction_id=r.origin_local_transaction_id;
  m.definition_version=r.generation; m.catalog_generation=r.catalog_generation; m.security_epoch=r.security_epoch;
  m.schema_epoch=m.resource_epoch=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active; m.status=c::CatalogObjectStatus::active;
  m.object_subtype="agent_runtime_authority"; m.trace_search_key="RUNTIME-BINDING-GATE"; m.retention_class="catalog_history";
  return m;
}
void Good(const Binding& r) {
  const auto expected=Oracle(r); const auto encoded=c::EncodeCatalogRuntimeAuthorityBinding(r);
  Check(encoded.ok() && std::string(encoded.bytes.begin(),encoded.bytes.end())==expected,"independent exact binary encoding");
  const auto decoded=c::DecodeCatalogRuntimeAuthorityBinding(expected);
  Check(decoded.ok() && Oracle(*decoded.record)==expected,"all values survive decode including optional reference");
  const auto outer=c::EncodeCatalogMetadataVersion(Metadata(r));
  Check(outer.ok(),"common catalog accepts actual typed binding");
  const auto read=c::DecodeCatalogMetadataVersion(outer.bytes);
  Check(read.ok() && read.record.record.payload==expected,"common metadata codec preserves binary definition");
}
void Bad(const Binding& r) {
  Check(!c::EncodeCatalogRuntimeAuthorityBinding(r).ok(),"invalid definition refused by encoder");
  Check(!c::DecodeCatalogRuntimeAuthorityBinding(Oracle(r)).ok(),"independent invalid definition refused by reader");
  Check(!c::EncodeCatalogMetadataVersion(Metadata(r)).ok(),"invalid definition refused by actual common metadata path");
}
void Run() {
  Good(Example());
  for (u64 mode : {1,2,3}) for (bool credential : {false,true}) {
    auto r=Example(); r.authority_mode=static_cast<c::RuntimeAuthorityMode>(mode);
    if (mode!=1) r.security_authority_uuid=Id(12);
    if (credential) r.credential_reference_uuid=Id(11);
    Good(r);
  }
  for (u64 mode : {u64{0},u64{4},std::numeric_limits<u64>::max()}) { auto r=Example(); r.authority_mode=static_cast<c::RuntimeAuthorityMode>(mode); Bad(r); }
  { auto r=Example(); r.security_authority_uuid=Id(99); Bad(r); }
  for (auto field : {&Binding::generation,&Binding::security_epoch,&Binding::policy_epoch,
       &Binding::provider_generation,&Binding::catalog_generation,&Binding::origin_local_transaction_id}) {
    auto r=Example(); r.*field=0; Bad(r);
    r.*field=std::numeric_limits<u64>::max(); Good(r);
  }
  for (auto field : {&Binding::binding_uuid,&Binding::database_uuid,&Binding::service_principal_uuid,
       &Binding::security_authority_uuid,&Binding::provider_uuid,&Binding::policy_uuid}) {
    auto r=Example(); r.*field={}; Bad(r);
    r=Example(); (r.*field).bytes[6]=0x40; Bad(r);
  }
  { auto r=Example(); r.credential_reference_uuid=Uuid{}; Bad(r); }
  { auto r=Example(); r.origin_transaction_uuid.kind=UuidKind::object; Check(!c::EncodeCatalogRuntimeAuthorityBinding(r).ok(),"transaction domain required"); }
  { auto r=Example(); r.origin_transaction_uuid.value={}; Bad(r); }
  for (bool credential : {false,true}) {
    auto r=Example(); if (credential) r.credential_reference_uuid=Id(11);
    const auto bytes=Oracle(r);
    for (std::size_t size=0; size<bytes.size(); ++size)
      Check(!c::DecodeCatalogRuntimeAuthorityBinding(bytes.substr(0,size)).ok(),"every truncated prefix rejected");
    Check(!c::DecodeCatalogRuntimeAuthorityBinding(bytes+"x").ok(),"trailing bytes rejected");
    for (std::size_t at : std::array<std::size_t,12>{0,4,6,8,12,16,20,22,24,26,27,28}) {
      auto changed=bytes; changed[at]^=0x40;
      Check(!c::DecodeCatalogRuntimeAuthorityBinding(changed).ok(),"malformed header/field rejected");
    }
  }
  const auto original=Metadata(Example());
  for (unsigned mutation=0; mutation<13; ++mutation) {
    auto m=original;
    switch(mutation) {
      case 0: m.record.header.kind=c::CatalogRecordKind::policy; break;
      case 1: m.record.header.object_uuid.value=Id(99); break;
      case 2: m.object_subtype="another_config"; break;
      case 3: m.authority_scope=c::CatalogAuthorityScope::cluster; break;
      case 4: m.visibility=c::CatalogVisibilityClass::public_metadata; break;
      case 5: m.owning_schema_uuid.value=Id(99); break;
      case 6: m.owner_uuid={}; break;
      case 7: m.audit_uuid={}; break;
      case 8: m.security_policy_uuid.value=Id(99); break;
      case 9: ++m.definition_version; break;
      case 10: ++m.catalog_generation; break;
      case 11: ++m.security_epoch; break;
      case 12: ++m.creator_local_transaction_id; break;
    }
    Check(!c::EncodeCatalogMetadataVersion(m).ok(),"common path refuses mismatched metadata");
  }
  auto successor=Example(); successor.generation=2; successor.service_principal_uuid=Id(20);
  auto replacement=Metadata(successor); replacement.creator_transaction_uuid={UuidKind::transaction,Id(21)};
  replacement.creator_local_transaction_id=23;
  Check(c::EncodeCatalogMetadataVersion(replacement).ok(),"replacement uses new creator retaining original origin");
  Check(c::CatalogMetadataPreservesFamilyOrigin(original,replacement),"actual family path accepts same-origin replacement");
  for(unsigned mutation=0; mutation<4; ++mutation) {
    auto r=successor;
    if(mutation==0) r.binding_uuid=Id(99);
    if(mutation==1) r.database_uuid=r.security_authority_uuid=Id(99);
    if(mutation==2) r.origin_transaction_uuid.value=Id(99);
    if(mutation==3) ++r.origin_local_transaction_id;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(original,Metadata(r)),"family path refuses origin substitution");
  }
  auto other=original; other.object_subtype="unrelated"; other.record.payload="not a startup definition";
  Check(!c::CatalogMetadataPreservesFamilyOrigin(original,other),"cannot morph binding into unrelated family");
  Check(c::CatalogMetadataPreservesFamilyOrigin(other,other),"unrelated configuration untouched");
}
} // namespace
int main() {
  try { Run(); std::cout << "runtime authority binding checks=" << checks << " failures=0\n"; return 0; }
  catch(const std::exception& error) { std::cerr << "FAIL " << error.what() << " checks=" << checks << '\n'; return 1; }
}
