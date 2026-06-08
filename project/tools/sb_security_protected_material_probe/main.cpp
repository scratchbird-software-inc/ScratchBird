// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/protected_material_api.hpp"

#include <filesystem>
#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {
EngineRequestContext Context(std::initializer_list<const char*> tags) {
  EngineRequestContext context;
  context.database_path = "/tmp/sb_security_protected_material_probe.sbdb";
  context.security_context_present = true;
  context.local_transaction_id = 8;
  for (const char* tag : tags) { context.trace_tags.emplace_back(tag); }
  return context;
}
}

int main() {
  std::filesystem::remove("/tmp/sb_security_protected_material_probe.sbdb");
  EngineRequestProtectedMaterialRequest release;
  release.context = Context({"right:PROTECTED_MATERIAL_RELEASE"});
  release.purpose = "backup_encrypt";
  const auto released = EngineRequestProtectedMaterial(release);

  EngineRequestProtectedMaterialRequest denied;
  denied.context = Context({});
  denied.purpose = "backup_encrypt";
  const auto denied_result = EngineRequestProtectedMaterial(denied);

  EngineRequestProtectedMaterialRequest wrong_key;
  wrong_key.context = Context({"right:PROTECTED_MATERIAL_RELEASE"});
  wrong_key.purpose = "backup_encrypt";
  wrong_key.option_envelopes.push_back("key_state:wrong");
  const auto wrong_result = EngineRequestProtectedMaterial(wrong_key);

  const bool ok = released.ok && released.released && !denied_result.ok && !wrong_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"released\":" << (released.released ? "true" : "false")
            << ",\"denied\":" << (!denied_result.ok ? "true" : "false")
            << ",\"wrong_key\":" << (!wrong_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
