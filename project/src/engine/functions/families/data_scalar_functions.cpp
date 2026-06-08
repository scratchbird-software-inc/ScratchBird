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

#include "data_scalar_functions_00_common_helpers.inc"
#include "data_scalar_functions_01_numeric_temporal.inc"
#include "data_scalar_functions_02_string_regex.inc"
#include "data_scalar_functions_03_uuid_crypto_bit.inc"
#include "data_scalar_functions_04_json_collection_range.inc"
#include "data_scalar_functions_05_conversion_conditional.inc"
#include "data_scalar_functions_06_system_session_catalog.inc"
#include "data_scalar_functions_07_dispatch_entrypoints.inc"
