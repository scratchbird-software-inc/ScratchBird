// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
// SBSQL_SOURCE_LAYOUT_FRAGMENT_WRAPPER
// This file is intentionally a thin ordered include wrapper created during
// source-layout hardening. Existing behavior is preserved by including the
// original source content in deterministic fragments.
// New SBSQL/SBLR work must be placed in the family-owned source files and
// registry fragments, not appended to this wrapper.

#include "sblr_aggregate_window_runtime_00_common_helpers.inc"
#include "sblr_aggregate_window_runtime_01_aggregate_general.inc"
#include "sblr_aggregate_window_runtime_02_aggregate_ordered_approx.inc"
#include "sblr_aggregate_window_runtime_03_window_kind_resolution.inc"
#include "sblr_aggregate_window_runtime_04_window_evaluation.inc"
