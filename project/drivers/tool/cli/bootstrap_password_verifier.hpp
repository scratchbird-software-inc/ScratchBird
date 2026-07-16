// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

namespace scratchbird::cli {

inline constexpr int kBootstrapPasswordPbkdf2Iterations = 600000;

bool BootstrapPasswordSecretValid(std::string_view password);
bool DeriveBootstrapPasswordVerifier(const std::string& password,
                                     std::string* envelope);

}  // namespace scratchbird::cli
