# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Pinned portable reference primitives only; no upstream heap/thread facade.
set(SB_ARGON2_PRIVATE_SYMBOLS init_block_value copy_block xor_block allocate_memory
  free_memory secure_wipe_memory clear_internal_memory finalize index_alpha
  fill_memory_blocks validate_inputs fill_first_blocks initial_hash initialize
  fill_segment fill_segment_interruptible blake2b_init blake2b_init_key
  blake2b_init_param blake2b_update blake2b_final blake2b blake2b_long)
set(SB_ARGON2_RENAMES)
foreach(symbol IN LISTS SB_ARGON2_PRIVATE_SYMBOLS)
  list(APPEND SB_ARGON2_RENAMES "${symbol}=sb_argon2_${symbol}")
endforeach()
add_library(sb_argon2_reference STATIC argon2_reference/src/core.c
  argon2_reference/src/ref.c argon2_reference/src/blake2/blake2b.c)
target_compile_features(sb_argon2_reference PRIVATE c_std_99)
target_compile_definitions(sb_argon2_reference PRIVATE ARGON2_NO_THREADS ${SB_ARGON2_RENAMES})
target_include_directories(sb_argon2_reference PRIVATE argon2_reference/include)
set_target_properties(sb_argon2_reference PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
set_source_files_properties(argon2_kdf.cpp PROPERTIES COMPILE_DEFINITIONS "${SB_ARGON2_RENAMES}")

add_library(sb_core_hash
  blake3_digest.cpp
  hash_digest.cpp
  crypto_memory_adapter.cpp
  scrypt_kdf.cpp
  openpgp_seipd.cpp
  openpgp_armor.cpp
  argon2_kdf.cpp
)

target_compile_features(sb_core_hash PUBLIC cxx_std_17)
target_include_directories(sb_core_hash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sb_core_hash PUBLIC sb_core_platform OpenSSL::Crypto)
target_link_libraries(sb_core_hash PRIVATE Threads::Threads sb_argon2_reference)
target_include_directories(sb_core_hash PRIVATE argon2_reference/include)
