// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <string>
#include <string_view>

namespace scratchbird::core::crypto {

// Single implementation of local-password verifier derivation.
//
// This lives in the core crypto layer because both the engine and the bootstrap
// tool need it and neither may depend on the other: the engine security layer
// cannot reach drivers/tool/cli, and the bootstrap tool links sb_storage_database
// rather than sb_engine_internal_api. Both reach sb_core_hash, so this is the
// one place a shared derivation can live without inverting a layer boundary.
//
// It exists as one implementation deliberately. Two copies of a KDF with
// independently maintained constants diverge silently, and a divergence makes
// credentials derived by one unverifiable by the other. There is no second copy
// to keep in step.
//
// Mechanism only. Deriving an envelope admits no principal, provider, session or
// transaction and grants no right, exactly as verifying one admits nothing.
//
// The caller owns the secret. This neither generates nor stores one, and refuses
// a secret that is too short, oversized or contains a NUL, so a verifier cannot
// be derived from an empty or placeholder value and presented as a credential.

inline constexpr int kLocalPasswordPbkdf2Iterations = 600000;
inline constexpr std::size_t kLocalPasswordSecretMinBytes = 12;
inline constexpr std::size_t kLocalPasswordSecretMaxBytes = 1024;

// A secret acceptable for derivation. Length and NUL-freedom only; this makes no
// judgement about entropy, which the caller owns.
bool LocalPasswordSecretValid(std::string_view secret) noexcept;

// On success writes
//   local-password-pbkdf2-sha256:v1:iterations=N:salt=<hex>:verifier=<hex>
// which is the envelope form the create path validates and the engine verifies.
// Salt and verifier are public encoded material; the secret is never copied into
// the output. Returns false without touching *envelope on invalid input or
// provider failure.
bool DeriveLocalPasswordVerifier(std::string_view secret, std::string* envelope);

}  // namespace scratchbird::core::crypto
