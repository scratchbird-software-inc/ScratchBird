# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(sb_core_hash
  blake3_digest.cpp
  hash_digest.cpp
  crypto_memory_adapter.cpp
  scrypt_kdf.cpp
)

target_compile_features(sb_core_hash PUBLIC cxx_std_17)
target_include_directories(sb_core_hash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_hash PUBLIC sb_core_platform OpenSSL::Crypto)
target_link_libraries(sb_core_hash PRIVATE Threads::Threads)
