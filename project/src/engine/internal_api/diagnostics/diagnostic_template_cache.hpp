// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IPAR-P6-26 diagnostic template cache.
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct IparDiagnosticTemplateEpoch {
  std::uint64_t diagnostic_epoch = 0;
  std::uint64_t language_epoch = 0;
  std::uint64_t policy_epoch = 0;
};

struct IparDiagnosticTemplate {
  std::string diagnostic_code;
  std::string message_key;
  std::string severity;
  std::string language_tag = "en";
  std::string public_shape_id = "diag.message_vector.v1";
  std::string private_shape_id = "diag.message_vector.v1.private";
  std::string localization_hook;
  std::vector<std::string> variable_names;
  IparDiagnosticTemplateEpoch epoch;
};

struct IparDiagnosticTemplateLookupResult {
  bool ok = false;
  bool cache_hit = false;
  bool stale = false;
  bool formatting_skipped = false;
  std::string diagnostic_code;
  std::string detail;
  IparDiagnosticTemplate entry;
};

struct IparDiagnosticRenderRequest {
  std::string diagnostic_code;
  std::string language_tag = "en";
  IparDiagnosticTemplateEpoch epoch;
  bool failure_path = true;
  std::map<std::string, std::string> variables;
};

struct IparDiagnosticRenderResult {
  bool ok = false;
  bool formatting_skipped = false;
  std::string diagnostic_code;
  std::string message_key;
  std::string severity;
  std::string rendered_message;
  std::string detail;
  std::vector<std::string> evidence;
};

class IparDiagnosticTemplateCache {
 public:
  IparDiagnosticTemplateLookupResult Put(IparDiagnosticTemplate entry);
  IparDiagnosticTemplateLookupResult Lookup(
      const std::string& diagnostic_code,
      const std::string& language_tag,
      IparDiagnosticTemplateEpoch epoch);
  IparDiagnosticRenderResult Render(IparDiagnosticRenderRequest request);
  std::uint64_t InvalidateStale(IparDiagnosticTemplateEpoch current_epoch);
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::map<std::string, IparDiagnosticTemplate> entries_;
};

bool IparDiagnosticTemplateEpochComplete(IparDiagnosticTemplateEpoch epoch);
bool IparDiagnosticTemplateEpochMatches(IparDiagnosticTemplateEpoch left,
                                        IparDiagnosticTemplateEpoch right);
std::string IparDiagnosticTemplateCacheKey(const std::string& diagnostic_code,
                                           const std::string& language_tag);

}  // namespace scratchbird::engine::internal_api
