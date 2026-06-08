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

#include "crud_store_00_common_types.inc"
#include "crud_store_01_relation_descriptor.inc"
#include "crud_store_02_row_encoding.inc"
#include "crud_store_03_mutation_support.inc"
#include "crud_store_04_predicate_filter.inc"
#include "crud_store_05_index_bulk_support.inc"
#include "crud_store_06_result_evidence_entrypoints.inc"
