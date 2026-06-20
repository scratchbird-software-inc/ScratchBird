// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_format.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {

namespace disk = scratchbird::storage::disk;

bool Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool SupportedSetIsExact() {
  constexpr std::array<scratchbird::core::platform::u32, 6> expected = {
      4096, 8192, 16384, 32768, 65536, 131072};
  return Expect(disk::kSupportedDatabasePageSizes == expected,
                "public first-release page-size profile changed unexpectedly");
}

bool SupportedSizesAreAccepted() {
  bool ok = true;
  for (const auto page_size : disk::kSupportedDatabasePageSizes) {
    ok = Expect(disk::IsSupportedDatabasePageSize(page_size),
                "supported page size was rejected") && ok;
    ok = Expect(static_cast<scratchbird::core::platform::u32>(disk::PageSizeProfileFor(page_size)) == page_size,
                "page-size profile conversion changed the value") && ok;
  }
  return ok;
}

bool UnsupportedSizesAreRejected() {
  constexpr std::array<scratchbird::core::platform::u32, 13> rejected = {
      0, 512, 1000, 1024, 1536, 2048, 12345,
      262144, 524288, 1048576, 1048577, 2097152, 4194304};
  bool ok = true;
  for (const auto page_size : rejected) {
    ok = Expect(!disk::IsSupportedDatabasePageSize(page_size),
                "unsupported page size was accepted") && ok;
    disk::DatabaseHeader header;
    header.page_size = page_size;
    const auto validation = disk::ValidateDatabaseHeader(header);
    ok = Expect(!validation.ok(),
                "database header validation accepted unsupported page size") && ok;
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = SupportedSetIsExact() && ok;
  ok = SupportedSizesAreAccepted() && ok;
  ok = UnsupportedSizesAreRejected() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
