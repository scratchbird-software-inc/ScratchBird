// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

namespace scratchbird::cli {

// DUPLICATION NOTICE. The same derivation exists engine-side as
// `scratchbird::engine::internal_api::DeriveLocalPasswordVerifier` in
// `src/engine/internal_api/security/local_password_credential.hpp`, added so the
// server can derive a verifier without depending on this tool layer. This tool
// keeps its own copy because `sb_cli_first_principal_bootstrap` links
// sb_storage_database and OpenSSL, not sb_engine_internal_api, and adding that
// dependency for deduplication alone would invert a layer boundary.
//
// Both copies MUST produce the identical envelope form and iteration count. A
// change to either without the other makes credentials derived by one
// unverifiable by the engine. Change both or neither.
inline constexpr int kBootstrapPasswordPbkdf2Iterations = 600000;

bool BootstrapPasswordSecretValid(std::string_view password);
bool DeriveBootstrapPasswordVerifier(const std::string& password,
                                     std::string* envelope);

}  // namespace scratchbird::cli
