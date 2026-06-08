// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_DIAGNOSTIC_H
#define SCRATCHBIRD_ENGINE_DIAGNOSTIC_H

#include "scratchbird/engine/error.h"
#include "scratchbird/engine/export.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_engine_string_view_t {
  const char* data;
  uint64_t size_bytes;
} sb_engine_string_view_t;

typedef struct sb_engine_diagnostic_view_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t numeric_code;
  sb_engine_diagnostic_severity_t severity;
  sb_engine_string_view_t symbolic_code;
  sb_engine_string_view_t message_key;
  sb_engine_string_view_t safe_detail;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_diagnostic_view_t;

typedef struct sb_engine_diagnostic_set_view_t {
  uint32_t struct_size;
  uint32_t abi_version;
  const sb_engine_diagnostic_view_t* diagnostics;
  uint64_t diagnostic_count;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_diagnostic_set_view_t;

#ifdef __cplusplus
}
#endif

#endif
