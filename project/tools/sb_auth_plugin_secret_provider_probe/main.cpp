// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../auth_provider_probe_common/probe_common.hpp"
int main(){using namespace sb_auth_probe; auto ok=Request<EngineRotateCredentialRequest>("scram_sha256"); auto ok_r=EngineRotateCredential(ok); auto plain=Request<EngineRotateCredentialRequest>("scram_sha256"); plain.option_envelopes.push_back("store_reusable_plaintext:true"); auto plain_r=EngineRotateCredential(plain); auto missing=Request<EngineRotateCredentialRequest>("scram_sha256"); missing.option_envelopes.clear(); missing.option_envelopes.push_back("provider:scram_sha256"); auto miss_r=EngineRotateCredential(missing); return Finish({{"rotated",ok_r.ok&&ok_r.rotated},{"plaintext_rejected",!plain_r.ok},{"missing_material_rejected",!miss_r.ok}});}
