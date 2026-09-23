// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/udr/runtime/sb_udr_runtime.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace runtime = scratchbird::udr::runtime;
void Check(bool value) { if (!value) std::abort(); }
runtime::UdrUuid observed;
runtime::UdrIdentityContext observed_context;
runtime::UdrCallResult Echo(const runtime::UdrCallInput& input) {
  observed = input.package_uuid;
  observed_context = input.identities;
  return {true, input.payload, "{}"};
}
runtime::UdrCallResult WrongIdentity(const runtime::UdrCallInput&) {
  return {true, {}, "{}", scratchbird::tests::FixtureUuid(1338, 99)};
}
runtime::UdrStatus Lifecycle(const runtime::UdrUuid& identity) {
  observed = identity;
  return {true, "UDR.OK", {}};
}
int main() {
  runtime::ResetRuntimeForTest();
  runtime::UdrPackageDescriptor package;
  package.package_uuid = scratchbird::tests::FixtureUuid(1338, 1);
  package.package_uuid.bytes[10] = 0;
  package.package_uuid.bytes[11] = '\n';
  package.package_uuid.bytes[12] = '|';
  package.package_uuid.bytes[13] = 255;
  package.package_name = "native_package";
  package.abi_version = "sb_udr_v1";
  package.source_revision = "test"; package.binary_hash = "test_hash";
  package.signature_policy = "test_policy"; package.trusted_cpp = true;
  package.entrypoints = {{"echo", "test", Echo}, {"wrong", "test", WrongIdentity}};
  package.init = Lifecycle; package.shutdown = Lifecycle;
  Check(runtime::RegisterPackage(package).ok);
  auto other = package; other.package_uuid.bytes.back() ^= 1;
  Check(runtime::RegisterPackage(other).ok);
  Check(runtime::FindPackageDescriptor(package.package_uuid)->package_uuid == package.package_uuid);
  Check(runtime::LoadPackage(package.package_uuid).ok && observed == package.package_uuid);
  runtime::UdrCallInput input{package.package_uuid, "echo", "payload", "session_uuid=forged_text"};
  input.identities.session_uuid = package.package_uuid;
  input.identities.connection_uuid = scratchbird::tests::FixtureUuid(1343, 2);
  input.identities.database_uuid = scratchbird::tests::FixtureUuid(1343, 3);
  input.identities.resolved_objects = {other.package_uuid, package.package_uuid};
  const auto result = runtime::InvokePackage(input);
  Check(observed_context.session_uuid == input.identities.session_uuid);
  Check(observed_context.connection_uuid == input.identities.connection_uuid);
  Check(observed_context.database_uuid == input.identities.database_uuid);
  Check(observed_context.resolved_objects == input.identities.resolved_objects);
  Check(result.ok && result.payload == "payload" && result.package_uuid == package.package_uuid);
  Check(observed == package.package_uuid);
  Check(!runtime::InvokePackage({package.package_uuid, "wrong", {}, {}}).ok);
  runtime::UdrInvocationLease lease;
  Check(runtime::AcquireInvocationRef(package.package_uuid, &lease).ok);
  auto moved = std::move(lease);
  Check(moved.held() && !lease.held());
  const auto blocked = runtime::UnregisterPackage(package.package_uuid);
  Check(!blocked.ok && blocked.identity == package.package_uuid && blocked.detail.empty());
  moved.Release();
  Check(runtime::UnregisterPackage(package.package_uuid).ok);
  Check(runtime::FindPackageDescriptor(other.package_uuid).has_value());
  Check(!runtime::FindPackageDescriptor(package.package_uuid).has_value());
  package.package_uuid = {};
  Check(!runtime::RegisterPackage(package).ok);
  runtime::ResetRuntimeForTest();
  std::cout << "UDR native registry regression PASS\n";
}
