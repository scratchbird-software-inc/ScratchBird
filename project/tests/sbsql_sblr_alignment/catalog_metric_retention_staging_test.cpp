// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse the native filespace/inventory/checkpoint fixture and independent page
// oracles without creating a second copy of their storage layout assumptions.
#define main NativeFilespaceRegressionMain
#include "filespace_page_zero_test.cpp"
#undef main
int main() {
  try {
    CanonicalCatalogVersionStaging(true);
    std::cout << "metric policy native staging checks=" << checks << " failures=0\n";
    return 0;
  } catch (const std::exception& error) {
    allocation_budget=-1;
    std::cerr << "FAIL " << error.what() << " checks=" << checks << '\n';
    return 1;
  }
}
