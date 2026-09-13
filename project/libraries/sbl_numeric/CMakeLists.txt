# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Every numeric build contains the mandatory reference backend. Host arithmetic
# and optional acceleration are not a substitute for a missing reference.
find_path(SBL_NUMERIC_MPFR_INCLUDE_DIR NAMES mpfr.h REQUIRED)
find_library(SBL_NUMERIC_MPFR_LIBRARY NAMES mpfr REQUIRED)
find_path(SBL_NUMERIC_GMP_INCLUDE_DIR NAMES gmp.h REQUIRED)
find_library(SBL_NUMERIC_GMP_LIBRARY NAMES gmp REQUIRED)
# The exact integer/decimal implementation retains its portable cpp_int
# dependency; MPFR replaces binary floating point, not those families.
find_path(SBL_NUMERIC_BOOST_INCLUDE_DIR NAMES boost/multiprecision/cpp_int.hpp
  HINTS "$ENV{BOOST_ROOT}/include" "$ENV{BOOST_INCLUDEDIR}" REQUIRED)

include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES
  "${SBL_NUMERIC_MPFR_INCLUDE_DIR};${SBL_NUMERIC_GMP_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES
  "${SBL_NUMERIC_MPFR_LIBRARY};${SBL_NUMERIC_GMP_LIBRARY}")
set(_sbl_numeric_reference_check "
  #include <mpfr.h>
  #if MPFR_VERSION < MPFR_VERSION_NUM(4, 2, 1)
  #error MPFR 4.2.1 or newer is required
  #endif
  #if __GNU_MP_VERSION < 6 || (__GNU_MP_VERSION == 6 && __GNU_MP_VERSION_MINOR < 2)
  #error GMP 6.2 or newer is required
  #endif
  int main() {
    mpfr_t value;
    mpfr_init2(value, 113);
    mpfr_set_ui(value, 1, MPFR_RNDN);
    const bool valid = mpfr_min_prec(value) == 1 && mpfr_buildopt_tls_p();
    mpfr_clear(value);
    mpfr_free_cache2(MPFR_FREE_LOCAL_CACHE);
    return valid ? 0 : 1;
  }")
# Recheck on reconfiguration: cached success from another provider is not proof.
unset(SBL_NUMERIC_REFERENCE_LINKS CACHE)
check_cxx_source_compiles("${_sbl_numeric_reference_check}" SBL_NUMERIC_REFERENCE_LINKS)
if(NOT SBL_NUMERIC_REFERENCE_LINKS)
  message(FATAL_ERROR "sbl_numeric requires the MPFR >=4.2.1 / GMP >=6.2 reference backend")
endif()
if(NOT CMAKE_CROSSCOMPILING)
  unset(SBL_NUMERIC_REFERENCE_TLS CACHE)
  check_cxx_source_runs("${_sbl_numeric_reference_check}" SBL_NUMERIC_REFERENCE_TLS)
  if(NOT SBL_NUMERIC_REFERENCE_TLS)
    message(FATAL_ERROR "sbl_numeric requires a working thread-local MPFR reference backend")
  endif()
endif()
cmake_pop_check_state()

add_library(sbl_numeric sbl_numeric.cpp real128_reference.cpp)
target_compile_features(sbl_numeric PUBLIC cxx_std_17)
target_include_directories(sbl_numeric PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(sbl_numeric PRIVATE
  ${SBL_NUMERIC_MPFR_INCLUDE_DIR} ${SBL_NUMERIC_GMP_INCLUDE_DIR}
  ${SBL_NUMERIC_BOOST_INCLUDE_DIR})
target_link_libraries(sbl_numeric PUBLIC
  ${SBL_NUMERIC_MPFR_LIBRARY} ${SBL_NUMERIC_GMP_LIBRARY})
