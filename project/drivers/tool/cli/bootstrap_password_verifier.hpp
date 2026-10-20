// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

namespace scratchbird::cli {

// The derivation is implemented once in the core crypto layer
// (`src/core/hash/local_password_verifier.hpp`); this interface is retained for
// existing tool callers and delegates to it.
//
// This header stays self-contained so that including it does not impose a
// core-crypto include path on every consumer. The value below is checked against
// the core constant by a static_assert in the implementation, so a divergence is
// a compile error rather than a silent credential incompatibility.
inline constexpr int kBootstrapPasswordPbkdf2Iterations = 600000;

bool BootstrapPasswordSecretValid(std::string_view password);
bool DeriveBootstrapPasswordVerifier(const std::string& password,
                                     std::string* envelope);

}  // namespace scratchbird::cli
