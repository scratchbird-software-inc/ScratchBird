// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_ERROR_H
#define SCRATCHBIRD_ENGINE_ERROR_H

#include "scratchbird/engine/export.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum sb_engine_status_t {
  SB_ENGINE_STATUS_OK = 0,
  SB_ENGINE_STATUS_INVALID_ARGUMENT = 1,
  SB_ENGINE_STATUS_INVALID_HANDLE = 2,
  SB_ENGINE_STATUS_UNSUPPORTED = 3,
  SB_ENGINE_STATUS_CAPABILITY_DISABLED = 4,
  SB_ENGINE_STATUS_SECURITY_DENIED = 5,
  SB_ENGINE_STATUS_TRANSACTION_ACTIVE = 6,
  SB_ENGINE_STATUS_TRANSACTION_REQUIRED = 7,
  SB_ENGINE_STATUS_CONFLICT = 8,
  SB_ENGINE_STATUS_NOT_FOUND = 9,
  SB_ENGINE_STATUS_TIMEOUT = 10,
  SB_ENGINE_STATUS_RESOURCE_EXHAUSTED = 11,
  SB_ENGINE_STATUS_INTERNAL_ERROR = 12,
  SB_ENGINE_STATUS_ALREADY_RELEASED = 13
} sb_engine_status_t;

typedef enum sb_engine_diagnostic_severity_t {
  SB_ENGINE_DIAGNOSTIC_INFO = 0,
  SB_ENGINE_DIAGNOSTIC_WARNING = 1,
  SB_ENGINE_DIAGNOSTIC_ERROR = 2
} sb_engine_diagnostic_severity_t;

SCRATCHBIRD_ENGINE_API const char* SCRATCHBIRD_ENGINE_CALL
sb_engine_status_name(sb_engine_status_t status);

#ifdef __cplusplus
}
#endif

#endif
