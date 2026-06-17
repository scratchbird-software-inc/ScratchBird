// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <span>
#include <string_view>

namespace scratchbird::engine::internal_api {

struct SbsqlLanguageElementCatalogRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view element_kind;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view sblr_operation_family;
  std::string_view support_state;
  std::string_view release_status;
  std::string_view predictive_state;
  std::string_view keyword_text;
  std::string_view keyword_class;
};

std::span<const SbsqlLanguageElementCatalogRow> SbsqlLanguageElementCatalogRows();

}  // namespace scratchbird::engine::internal_api
