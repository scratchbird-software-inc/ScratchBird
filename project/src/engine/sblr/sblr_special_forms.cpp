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

#include "sblr_special_forms_00_common_helpers.inc"
#include "sblr_special_forms_01_cast_conversion.inc"
#include "sblr_special_forms_02_conditional_forms.inc"
#include "sblr_special_forms_03_sequence_session_forms.inc"
#include "sblr_special_forms_04_diagnostic_refusal_forms.inc"
