# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

add_library(sbl_numeric
  sbl_numeric.cpp
)

include(CheckCXXSourceCompiles)

set(SBL_NUMERIC_SAVED_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
set(CMAKE_REQUIRED_LIBRARIES quadmath)
check_cxx_source_compiles("
  #include <quadmath.h>
  int main() {
    __float128 value = strtoflt128(\"1.0\", nullptr);
    char buffer[64];
    return quadmath_snprintf(buffer, sizeof(buffer), \"%Qg\", value) < 0;
  }
" SBL_NUMERIC_HAS_QUADMATH)
set(CMAKE_REQUIRED_LIBRARIES "${SBL_NUMERIC_SAVED_REQUIRED_LIBRARIES}")

target_compile_features(sbl_numeric PUBLIC cxx_std_17)
target_include_directories(sbl_numeric PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(SBL_NUMERIC_HAS_QUADMATH)
  target_compile_definitions(sbl_numeric PUBLIC SBL_NUMERIC_HAS_QUADMATH=1)
  target_link_libraries(sbl_numeric PUBLIC quadmath)
elseif(SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE)
  message(FATAL_ERROR
    "release-complete non-cluster engine requires sbl_numeric real128 backend support through quadmath")
endif()
