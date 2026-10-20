// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_password_verifier.hpp"

#include "local_password_verifier.hpp"

namespace scratchbird::cli {

// A divergence here would make credentials derived by this tool unverifiable by
// the engine. Caught at compile time rather than trusted to review.
static_assert(kBootstrapPasswordPbkdf2Iterations ==
                  core::crypto::kLocalPasswordPbkdf2Iterations,
              "bootstrap and core local-password iteration counts must match");

// Thin delegation. The derivation itself is implemented once in the core crypto
// layer, which both this tool and the engine reach; keeping a second copy here
// risked the two diverging and making credentials derived by one unverifiable by
// the other.
bool BootstrapPasswordSecretValid(std::string_view password) {
  return core::crypto::LocalPasswordSecretValid(password);
}

bool DeriveBootstrapPasswordVerifier(const std::string& password,
                                     std::string* envelope) {
  return core::crypto::DeriveLocalPasswordVerifier(password, envelope);
}

}  // namespace scratchbird::cli
