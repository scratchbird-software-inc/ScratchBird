// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "native_sblr_specialization.hpp"

namespace scratchbird::engine::sblr {

// SEARCH_KEY: SB_SBLR_NATIVE_SPECIALIZATION_ODF_104
// SBLR exposes optional native specialization only through stable prepared
// template identity and exact scalar fallback. It does not transfer
// transaction, visibility, security, redaction, parser, or reference authority to
// the native provider.

using SblrNativeSpecializationKind =
    scratchbird::engine::native_compile::NativeSblrSpecializationKind;
using SblrNativeSpecializationRequest =
    scratchbird::engine::native_compile::NativeSblrSpecializationRequest;
using SblrNativeSpecializationResult =
    scratchbird::engine::native_compile::NativeSblrSpecializationResult;

SblrNativeSpecializationResult ExecuteSblrNativeSpecialization(
    const SblrNativeSpecializationRequest& request);

}  // namespace scratchbird::engine::sblr
