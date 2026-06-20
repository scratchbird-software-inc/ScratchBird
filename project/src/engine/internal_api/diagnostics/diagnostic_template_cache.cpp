// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "diagnostics/diagnostic_template_cache.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kAnchor = "IPAR-P6-26_DIAGNOSTIC_TEMPLATE_CACHE";
constexpr const char* kAuthorityScope =
    "diagnostic_template_cache.failure_formatting_only_no_success_path_or_transaction_authority";

IparDiagnosticTemplateLookupResult Refusal(std::string code,
                                           std::string detail) {
  IparDiagnosticTemplateLookupResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

std::string ReplaceAll(std::string value,
                       const std::string& token,
                       const std::string& replacement) {
  std::size_t pos = 0;
  while ((pos = value.find(token, pos)) != std::string::npos) {
    value.replace(pos, token.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

}  // namespace

bool IparDiagnosticTemplateEpochComplete(IparDiagnosticTemplateEpoch epoch) {
  return epoch.diagnostic_epoch != 0 &&
         epoch.language_epoch != 0 &&
         epoch.policy_epoch != 0;
}

bool IparDiagnosticTemplateEpochMatches(IparDiagnosticTemplateEpoch left,
                                        IparDiagnosticTemplateEpoch right) {
  return left.diagnostic_epoch == right.diagnostic_epoch &&
         left.language_epoch == right.language_epoch &&
         left.policy_epoch == right.policy_epoch;
}

std::string IparDiagnosticTemplateCacheKey(const std::string& diagnostic_code,
                                           const std::string& language_tag) {
  return diagnostic_code + "|" + language_tag;
}

IparDiagnosticTemplateLookupResult IparDiagnosticTemplateCache::Put(
    IparDiagnosticTemplate entry) {
  if (entry.diagnostic_code.empty() ||
      entry.message_key.empty() ||
      entry.severity.empty() ||
      entry.language_tag.empty() ||
      !IparDiagnosticTemplateEpochComplete(entry.epoch)) {
    return Refusal("SB_IPAR_DIAGNOSTIC_TEMPLATE.INVALID",
                   "diagnostic_code_message_key_severity_language_and_epoch_required");
  }
  const std::string key =
      IparDiagnosticTemplateCacheKey(entry.diagnostic_code, entry.language_tag);
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[key] = std::move(entry);
  IparDiagnosticTemplateLookupResult result;
  result.ok = true;
  result.diagnostic_code = "SB_IPAR_DIAGNOSTIC_TEMPLATE.PUT";
  return result;
}

IparDiagnosticTemplateLookupResult IparDiagnosticTemplateCache::Lookup(
    const std::string& diagnostic_code,
    const std::string& language_tag,
    IparDiagnosticTemplateEpoch epoch) {
  if (diagnostic_code.empty() ||
      language_tag.empty() ||
      !IparDiagnosticTemplateEpochComplete(epoch)) {
    return Refusal("SB_IPAR_DIAGNOSTIC_TEMPLATE.REQUEST_INVALID",
                   "diagnostic_code_language_and_epoch_required");
  }
  const std::string key =
      IparDiagnosticTemplateCacheKey(diagnostic_code, language_tag);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return Refusal("SB_IPAR_DIAGNOSTIC_TEMPLATE.MISS", "template_not_cached");
  }
  if (!IparDiagnosticTemplateEpochMatches(found->second.epoch, epoch)) {
    auto result = Refusal("SB_IPAR_DIAGNOSTIC_TEMPLATE.STALE", "epoch_mismatch");
    result.stale = true;
    return result;
  }
  IparDiagnosticTemplateLookupResult result;
  result.ok = true;
  result.cache_hit = true;
  result.diagnostic_code = "SB_IPAR_DIAGNOSTIC_TEMPLATE.HIT";
  result.entry = found->second;
  return result;
}

IparDiagnosticRenderResult IparDiagnosticTemplateCache::Render(
    IparDiagnosticRenderRequest request) {
  IparDiagnosticRenderResult rendered;
  rendered.evidence.push_back(kAnchor);
  rendered.evidence.push_back(kAuthorityScope);
  if (!request.failure_path) {
    rendered.ok = true;
    rendered.formatting_skipped = true;
    rendered.diagnostic_code = "SB_IPAR_DIAGNOSTIC_TEMPLATE.SUCCESS_PATH_SKIPPED";
    rendered.detail = "success_path_diagnostic_formatting_skipped";
    return rendered;
  }

  auto lookup = Lookup(request.diagnostic_code,
                       request.language_tag,
                       request.epoch);
  if (!lookup.ok) {
    rendered.diagnostic_code = lookup.diagnostic_code;
    rendered.detail = lookup.detail;
    return rendered;
  }

  rendered.ok = true;
  rendered.diagnostic_code = lookup.entry.diagnostic_code;
  rendered.message_key = lookup.entry.message_key;
  rendered.severity = lookup.entry.severity;
  rendered.rendered_message = lookup.entry.message_key;
  for (const auto& name : lookup.entry.variable_names) {
    const auto found = request.variables.find(name);
    rendered.rendered_message =
        ReplaceAll(rendered.rendered_message,
                   "{" + name + "}",
                   found == request.variables.end() ? "" : found->second);
  }
  rendered.evidence.push_back("diagnostic_template_cache.failure_formatting=true");
  rendered.evidence.push_back("diagnostic_template_cache.localization_hook=" +
                              lookup.entry.localization_hook);
  return rendered;
}

std::uint64_t IparDiagnosticTemplateCache::InvalidateStale(
    IparDiagnosticTemplateEpoch current_epoch) {
  std::uint64_t count = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (IparDiagnosticTemplateEpochMatches(it->second.epoch, current_epoch)) {
      ++it;
      continue;
    }
    it = entries_.erase(it);
    ++count;
  }
  return count;
}

void IparDiagnosticTemplateCache::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

}  // namespace scratchbird::engine::internal_api
