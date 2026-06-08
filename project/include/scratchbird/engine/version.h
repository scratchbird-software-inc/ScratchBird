// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_VERSION_H
#define SCRATCHBIRD_ENGINE_VERSION_H

/* PUBLIC_API_ABI_SURFACE */

#include "scratchbird/engine/export.h"
#include "scratchbird/engine/error.h"

#include <stdint.h>

#define SB_ENGINE_ABI_VERSION_MAJOR 1u
#define SB_ENGINE_ABI_VERSION_MINOR 0u
#define SB_ENGINE_ABI_VERSION_PATCH 0u
#define SB_ENGINE_ABI_VERSION_PACKED \
  ((uint32_t)((SB_ENGINE_ABI_VERSION_MAJOR << 16u) | \
              (SB_ENGINE_ABI_VERSION_MINOR << 8u) | \
              SB_ENGINE_ABI_VERSION_PATCH))

SCRATCHBIRD_ENGINE_EXTERN_C SCRATCHBIRD_ENGINE_API uint32_t SCRATCHBIRD_ENGINE_CALL
sb_engine_abi_version_packed(void);

SCRATCHBIRD_ENGINE_EXTERN_C SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_abi_build_id(const char** out_data, uint64_t* out_size);

#endif
