// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/security/authentication_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace api = scratchbird::engine::internal_api;
void Check(bool value) { if (!value) std::abort(); }
int main() {
  auto uuid=scratchbird::tests::FixtureUuid(1306,1);
  uuid.bytes[9]=0;uuid.bytes[10]='\n';uuid.bytes[11]=255;
  api::EngineSecurityPrincipalRecord record;
  record.principal_uuid=uuid;record.principal_name="alice";record.lifecycle_state="active";
  api::EngineSecurityPrincipalLifecycleState state;state.principals={record};
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"alice",{})==uuid);
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"alice",uuid)==uuid);
  auto other=uuid;other.bytes[15]^=1;
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"alice",other).is_nil());
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"bob",uuid).is_nil());
  state.principals[0].deleted=true;
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"alice",uuid).is_nil());
  state.principals[0].deleted=false;state.principals.push_back(record);
  Check(api::ResolveAuthenticatedPrincipalIdentity(state,"alice",uuid).is_nil());
  api::EngineAuthenticateRequest request;request.durable_principal_uuid=uuid;
  Check(api::DurablePrincipalUuid(request,{})==uuid);
  Check(api::DurablePrincipalUuid(request,{{"principal_uuid","text"}}).is_nil());
  request.option_envelopes={"durable_principal_uuid:text"};
  Check(api::DurablePrincipalUuid(request,{}).is_nil());
  std::cout<<"authentication native principal binding regression PASS\n";
}
