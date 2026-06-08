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

#include "sblr_operator_runtime_00_common_helpers.inc"
#include "sblr_operator_runtime_01_numeric_arithmetic.inc"
#include "sblr_operator_runtime_02_temporal_interval.inc"
#include "sblr_operator_runtime_03_text_binary.inc"
#include "sblr_operator_runtime_04_boolean_comparison.inc"
#include "sblr_operator_runtime_05_pattern_matching.inc"
#include "sblr_operator_runtime_06_json_collection_range.inc"
#include "sblr_operator_runtime_07_bit_vector_search.inc"
#include "sblr_operator_runtime_08_registry_entries.inc"
#include "sblr_operator_runtime_09_public_entrypoints.inc"
