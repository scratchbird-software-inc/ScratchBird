// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/temporary_name_visibility.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "api_diagnostics.hpp"
#include <iostream>

namespace api=scratchbird::engine::internal_api;
namespace {
unsigned checks=0,failures=0;
void Check(bool valid,const char* why){++checks;if(!valid){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
api::EngineUuid Id(unsigned suffix) {
  api::EngineUuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=suffix;return id;
}
}
int main() {
  using K=api::TemporaryNameCandidateKind;
  api::EngineRequestContext context;context.session_uuid=Id(1);
  api::MgaTemporaryTableVisibilityResult observation;
  observation.ok=true;observation.table_visible=true;observation.visible_to_session=true;
  observation.diagnostic=api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
  observation.table.table_uuid=Id(2);
  const auto classify=[&](const auto& seen,const auto& owner,const auto& object){
    return api::ClassifyTemporaryNameCandidate(owner,object,seen);
  };
  const auto expect=[&](const auto& seen,K kind){
    const auto result=classify(seen,context,Id(2));
    Check(result.ok && result.kind==kind && !result.diagnostic.error &&
          result.diagnostic.occurrence_uuid==seen.diagnostic.occurrence_uuid,
          "valid classification changed or lost its source outcome");
  };
  const auto refuse=[&](const auto& seen,const auto& owner,const auto& object){
    const auto result=classify(seen,owner,object);
    Check(!result.ok && result.kind==K::kNotVisible && result.diagnostic.error &&
          !result.diagnostic.code.empty() && result.diagnostic.canonical_metadata.has_value(),
          "invalid namespace published a visible candidate or lost canonical diagnostic");
  };
  expect(observation,K::kCatalog);
  auto bad=observation;bad.table.temporary_scope="private";refuse(bad,context,Id(2));
  bad=observation;bad.table.temporary_session_uuid=Id(1);refuse(bad,context,Id(2));
  auto global=observation;global.table.temporary=true;global.known_temporary=true;
  global.table.temporary_scope="global";
  expect(global,K::kCatalog);
  bad=global;bad.visible_to_session=false;refuse(bad,context,Id(2));
  bad=global;bad.table.temporary_session_uuid=Id(1);refuse(bad,context,Id(2));
  auto private_table=global;private_table.table.temporary_scope="private";
  private_table.table.temporary_session_uuid=context.session_uuid;
  expect(private_table,K::kOwnedPrivate);
  bad=private_table;bad.visible_to_session=false;expect(bad,K::kNotVisible);
  bad=private_table;bad.table.temporary_session_uuid=Id(3);expect(bad,K::kNotVisible);
  bad.visible_to_session=false;expect(bad,K::kNotVisible);
  for(bool known:{false,true})for(bool hidden:{false,true}) {
    bad=private_table;bad.table_visible=false;bad.known_temporary=known;
    bad.hidden_by_temporary_visibility=hidden;
    expect(bad,known||hidden?K::kNotVisible:K::kCatalog);
  }
  for(const auto* scope:{"", "PRIVATE", "session", "invalid"}) {
    bad=private_table;bad.table.temporary_scope=scope;refuse(bad,context,Id(2));
  }
  bad=private_table;bad.table.temporary_session_uuid={};refuse(bad,context,Id(2));
  auto invalid_context=context;invalid_context.session_uuid={};refuse(private_table,invalid_context,Id(2));
  refuse(private_table,context,api::EngineUuid{});
  for(unsigned version=0;version<16;++version)if(version!=7) {
    auto invalid=Id(1);invalid.bytes[6]=static_cast<unsigned char>(version<<4);
    bad=private_table;bad.table.temporary_session_uuid=invalid;refuse(bad,context,Id(2));
    invalid_context=context;invalid_context.session_uuid=invalid;refuse(private_table,invalid_context,Id(2));
    refuse(private_table,context,invalid);
  }
  for(unsigned variant:{0u,0x40u,0xc0u}) {
    auto invalid=Id(1);invalid.bytes[8]=variant;
    bad=private_table;bad.table.temporary_session_uuid=invalid;refuse(bad,context,Id(2));
    invalid_context=context;invalid_context.session_uuid=invalid;refuse(private_table,invalid_context,Id(2));
    refuse(private_table,context,invalid);
  }
  // No textual UUID prefix/suffix comparison: every identity byte participates.
  for(unsigned position=0;position<16;++position) {
    bad=private_table;bad.table.table_uuid.bytes[position]^=1;refuse(bad,context,Id(2));
    auto other=context;other.session_uuid.bytes[position]^=1;
    const auto result=classify(private_table,other,Id(2));
    Check(result.ok && result.kind==K::kNotVisible,"another session inherited private namespace visibility");
  }
  bad=private_table;bad.ok=false;
  bad.diagnostic=api::MakeEngineApiDiagnostic(
      "CATALOG.INVALID_INPUT","fixture.visibility","source failure");
  const auto failed=classify(bad,context,Id(2));
  Check(!failed.ok && failed.kind==K::kNotVisible &&
        failed.diagnostic.occurrence_uuid==bad.diagnostic.occurrence_uuid &&
        failed.diagnostic.code==bad.diagnostic.code && failed.diagnostic.detail==bad.diagnostic.detail,
        "visibility failure lost its source diagnostic identity");
  bad.ok=true;refuse(bad,context,Id(2));
  bad.diagnostic={};refuse(bad,context,Id(2));
  bad.ok=false;refuse(bad,context,Id(2));
  std::cout<<"temporary_name_visibility checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
