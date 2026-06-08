// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-LOCALIZATION-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;

enum class IndexLocalizedTextClass : u32 {
  name = 1,
  comment = 2,
  donor_catalog_label = 3,
  diagnostic_label = 4
};

struct IndexLocalizedText {
  TypedUuid index_uuid;
  std::string language_tag;
  std::string path;
  IndexLocalizedTextClass text_class = IndexLocalizedTextClass::name;
  std::string value;
  bool is_default = false;
};

struct IndexLocalizedProjectionRequest {
  TypedUuid index_uuid;
  std::string requested_language_tag;
  std::string default_language_tag = "en";
  std::vector<IndexLocalizedText> texts;
};

struct IndexLocalizedProjectionResult {
  Status status;
  TypedUuid index_uuid;
  std::string language_tag;
  std::string name;
  std::string comment;
  bool used_default_language = false;
  bool ambiguity_detected = false;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !ambiguity_detected; }
};

IndexLocalizedProjectionResult ResolveIndexLocalizedProjection(const IndexLocalizedProjectionRequest& request);
DiagnosticRecord MakeIndexLocalizationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::index
