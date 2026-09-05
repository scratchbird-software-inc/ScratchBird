// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"

int main() {
  using namespace sb_auth_probe;
  auto valid = Request<EngineRotateCredentialRequest>("scram_sha256");
  const auto valid_r = EngineRotateCredential(valid);

  auto plaintext = valid;
  plaintext.option_envelopes.push_back("store_reusable_plaintext:true");
  const auto plaintext_r = EngineRotateCredential(plaintext);

  auto missing = valid;
  RemoveOption(&missing, "protected_material:available");
  const auto missing_r = EngineRotateCredential(missing);

  return Finish({
      {"rotated", valid_r.ok && valid_r.rotated},
      {"plaintext_rejected",
       HasDiagnostic(plaintext_r, "SECURITY.PROTECTED_MATERIAL.DENIED",
                     "reusable_plaintext_forbidden")},
      {"missing_material_rejected",
       HasDiagnostic(missing_r, "SECURITY.KEY.UNAVAILABLE",
                     "protected_material_required_for_rotation")},
  });
}
