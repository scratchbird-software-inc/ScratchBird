// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

// Owns bounded scanning of already-canonical JSON used by query operators. It
// does not parse SQL, resolve document providers, or publish query results.
struct CanonicalDocumentWildcardExpansion {
  bool ok{false};
  bool path_present{false};
  std::vector<std::string> elements;
  std::string detail;
};

CanonicalDocumentWildcardExpansion ExpandCanonicalDocumentWildcard(
    std::string_view canonical_json,
    std::string_view path,
    std::size_t maximum_rows,
    std::uint64_t maximum_memory_bytes);

}  // namespace scratchbird::engine::sblr
