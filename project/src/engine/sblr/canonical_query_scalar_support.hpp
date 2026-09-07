// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

// Owns query-local canonical UUID derivation and scalar wire normalization.
// It does not compare catalog-bound values or decide descriptor authority.
bool CanonicalUuidText(std::string_view value);

std::string DerivedCanonicalUuid(std::string_view scope,
                                 std::string_view purpose);

bool EncodeCanonicalScalarEqualityKey(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    std::string* key,
    std::string* refusal_detail);

bool DecodeCanonicalInt64Scalar(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    std::int64_t* decoded,
    std::string* refusal_detail);

}  // namespace scratchbird::engine::sblr
