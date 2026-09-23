// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/security/protected_material_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace api = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) {
  auto id = scratchbird::tests::FixtureUuid(1216,n);
  id.bytes[9]=255; id.bytes[10]=0; return id;
}
int main() {
  api::ProtectedMaterialCatalogImage catalog;
  api::EngineProtectedMaterialCatalogEntry material;
  material.database_uuid=Id(1); material.protected_material_uuid=Id(2);
  material.owner_scope_uuid=Id(3); material.active_version_uuid=Id(4);
  material.catalog_generation_id=1; material.purpose_class="security_use";
  material.storage_class="external_reference";
  material.policy.retention_policy_uuid=Id(5); material.policy.access_policy_uuid=Id(6);
  material.policy.release_policy_uuid=Id(7); material.policy.purge_policy_uuid=Id(8);
  material.policy.audit_policy_uuid=Id(9);
  material.policy.release_purposes={"security_use"};
  catalog.materials.push_back(material);
  api::EngineProtectedMaterialVersionCatalogEntry version;
  version.database_uuid=Id(1); version.protected_material_uuid=Id(2);
  version.protected_material_version_uuid=Id(4); version.policy=material.policy;
  version.version_number=1; version.catalog_generation_id=1;
  version.protected_reference="reference|with\nseparators";
  catalog.versions.push_back(version);
  api::EngineRequestContext context; context.database_uuid=Id(1); context.principal_uuid=Id(3);
  const auto audit=api::AppendMaterialAuditLocked(context,Id(1),Id(2),Id(4),"create","allow","ok","redacted",1);
  const auto second=api::AppendMaterialAuditLocked(context,Id(1),Id(2),Id(4),"create","allow","ok","redacted",1);
  Check(!audit.audit_event_uuid.is_nil() && audit.audit_event_uuid!=second.audit_event_uuid);
  catalog.audit_events.push_back(audit);
  const auto encoded=api::EncodeProtectedMaterialCatalog(catalog);
  Check(!encoded.empty() && encoded[7]=='2');
  auto decoded=api::DecodeProtectedMaterialCatalogBytes(encoded,Id(1));
  Check(decoded.ok && decoded.image.materials.size()==1 && decoded.image.audit_events.size()==1);
  Check(decoded.image.materials[0].protected_material_uuid==Id(2));
  Check(decoded.image.versions[0].protected_material_version_uuid==Id(4));
  Check(decoded.image.versions[0].protected_reference==version.protected_reference);
  Check(decoded.image.audit_events[0].audit_event_uuid==audit.audit_event_uuid);
  Check(api::EncodeProtectedMaterialCatalog(decoded.image)==encoded);
  Check(!api::DecodeProtectedMaterialCatalogBytes(encoded,Id(99)).ok);
  for (std::size_t n=0;n<encoded.size();++n) {
    Check(!api::DecodeProtectedMaterialCatalogBytes(std::vector<api::byte>(encoded.begin(),encoded.begin()+n),Id(1)).ok);
  }
  auto bad=encoded; bad.push_back(0); Check(!api::DecodeProtectedMaterialCatalogBytes(bad,Id(1)).ok);
  bad=encoded; bad[7]='1'; Check(!api::DecodeProtectedMaterialCatalogBytes(bad,Id(1)).ok);
  bad=encoded; bad.back()^=1; Check(!api::DecodeProtectedMaterialCatalogBytes(bad,Id(1)).ok);
  catalog.audit_events[0].audit_event_uuid={}; Check(api::EncodeProtectedMaterialCatalog(catalog).empty());
  std::vector<api::byte> identity;
  Check(api::AppendIdentity(&identity,Id(1)) && identity.size()==16);
  api::EngineUuid restored; std::size_t offset=0;
  Check(api::ReadIdentity(identity,&offset,&restored) && offset==16 && restored==Id(1));
  const std::string human="019f0600-0000-7000-8000-000000000001";
  identity.assign(human.begin(),human.end()); offset=0;
  Check(!api::ReadIdentity(identity,&offset,&restored));
  api::EngineApiRequest request;
  request.option_envelopes={"key_uuid:"+api::MetadataUuidBytes(Id(1))};
  Check(!api::ValidateEngineAuthorityBoundary(request,"test").error);
  request.option_envelopes.push_back(request.option_envelopes.front());
  Check(api::ValidateEngineAuthorityBoundary(request,"test").error);
  request.option_envelopes={"key_uuid:"+human};
  Check(api::ValidateEngineAuthorityBoundary(request,"test").error);
  std::cout << "protected material native codec passed\n";
}
