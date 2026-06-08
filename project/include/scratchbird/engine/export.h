// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_EXPORT_H
#define SCRATCHBIRD_ENGINE_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(SCRATCHBIRD_ENGINE_BUILDING_SHARED)
#    define SCRATCHBIRD_ENGINE_API __declspec(dllexport)
#  elif defined(SCRATCHBIRD_ENGINE_USING_SHARED)
#    define SCRATCHBIRD_ENGINE_API __declspec(dllimport)
#  else
#    define SCRATCHBIRD_ENGINE_API
#  endif
#  define SCRATCHBIRD_ENGINE_CALL __cdecl
#else
#  if defined(SCRATCHBIRD_ENGINE_BUILDING_SHARED)
#    define SCRATCHBIRD_ENGINE_API __attribute__((visibility("default")))
#  else
#    define SCRATCHBIRD_ENGINE_API
#  endif
#  define SCRATCHBIRD_ENGINE_CALL
#endif

#ifdef __cplusplus
#  define SCRATCHBIRD_ENGINE_EXTERN_C extern "C"
#else
#  define SCRATCHBIRD_ENGINE_EXTERN_C
#endif

#endif
