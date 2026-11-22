// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_PARSER_NAME_RESOLUTION (Part B step S1). See the header for
// the rules and the engine source each one mirrors.

#include "name_resolution/name_resolution.hpp"

#include <algorithm>

namespace scratchbird::parsers::name_resolution {

namespace {

class SbsqlFold final : public NameFoldRule {
 public:
  std::string_view Label() const noexcept override { return "sbsql_v3"; }
  std::string Key(std::string_view spelling, bool quoted) const override {
    return SbsqlIdentifierKey(spelling, quoted);
  }
};

bool EqualsAsciiNoCase(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

void PushUnique(std::vector<Uuid16>* list, const Uuid16& value) {
  if (std::find(list->begin(), list->end(), value) == list->end()) list->push_back(value);
}

}  // namespace

const NameFoldRule& SbsqlNameFoldRule() noexcept {
  static const SbsqlFold rule;
  return rule;
}

// Mirrors server/ipc_server.cpp PsNameSplitPresentedName, line for line.
std::optional<std::vector<NameComponent>> SplitPresentedName(std::string_view presented_name,
                                                             bool request_quoted) {
  std::vector<NameComponent> parts;
  NameComponent current;
  bool in_quote = false;
  bool saw_quote = false;
  for (std::size_t i = 0; i < presented_name.size(); ++i) {
    const char ch = presented_name[i];
    if (ch == '"') {
      if (in_quote && i + 1 < presented_name.size() && presented_name[i + 1] == '"') {
        current.spelling.push_back('"');
        ++i;
        continue;
      }
      in_quote = !in_quote;
      saw_quote = true;
      current.quoted = true;
      continue;
    }
    if (ch == '.' && !in_quote) {
      if (current.spelling.empty()) return std::nullopt;
      current.quoted = current.quoted || request_quoted;
      parts.push_back(std::move(current));
      current = {};
      saw_quote = false;
      continue;
    }
    current.spelling.push_back(ch);
  }
  if (in_quote || current.spelling.empty()) return std::nullopt;
  current.quoted = current.quoted || request_quoted || saw_quote;
  parts.push_back(std::move(current));
  return parts;
}

std::string RenderPresentedName(const std::vector<NameComponent>& components) {
  std::string out;
  for (const auto& component : components) {
    if (!out.empty()) out.push_back('.');
    if (!component.quoted) {
      out.append(component.spelling);
      continue;
    }
    out.push_back('"');
    for (const char ch : component.spelling) {
      if (ch == '"') out.push_back('"');
      out.push_back(ch);
    }
    out.push_back('"');
  }
  return out;
}

PreparedWireName PrepareWireName(const NameFoldRule& rule, std::string_view presented_name, bool quoted,
                                 std::string_view object_class) {
  PreparedWireName prepared;
  prepared.presented_name = std::string(presented_name);
  prepared.quoted = quoted;
  prepared.original = std::string(presented_name);
  if (EqualsAsciiNoCase(object_class, "charset") || EqualsAsciiNoCase(object_class, "collation")) return prepared;
  auto components = SplitPresentedName(presented_name, quoted);
  if (!components) return prepared;
  for (auto& component : *components) component.spelling = rule.Key(component.spelling, component.quoted);
  prepared.presented_name = RenderPresentedName(*components);
  prepared.rewritten = prepared.presented_name != prepared.original;
  return prepared;
}

PreparedWireName PrepareSbsqlWireName(std::string_view presented_name, bool quoted, std::string_view object_class) {
  return PrepareWireName(SbsqlNameFoldRule(), presented_name, quoted, object_class);
}

std::string OwnLane(const NameFoldRule& rule, const NameEntry& entry) {
  return rule.Key(entry.raw_text, entry.was_quoted);
}

bool NamesCollide(const NameFoldRule& rule, const NameEntry& a, const NameEntry& b) {
  return OwnLane(rule, a) == OwnLane(rule, b);
}

const std::vector<NameEntry>& NameListCache::Page(const Uuid16& scope, const std::string& language) {
  const auto key = std::make_pair(scope, language);
  auto found = pages_.find(key);
  if (found == pages_.end()) {
    ++source_reads_;
    found = pages_.emplace(key, source_->List(scope, language)).first;
  }
  return found->second;
}

void NameListCache::ObserveGeneration(std::uint64_t generation) {
  if (generation_ && *generation_ != generation) pages_.clear();
  generation_ = generation;
}

std::vector<std::string> LanguageCandidates(const ResolutionContext& context) {
  const std::string default_language = context.default_language.empty() ? "en" : context.default_language;
  const std::string session_language = context.session_language.empty() ? default_language : context.session_language;
  std::vector<std::string> languages;
  for (const auto* value : {&session_language, &default_language}) {
    if (!value->empty() && std::find(languages.begin(), languages.end(), *value) == languages.end())
      languages.push_back(*value);
  }
  if (std::find(languages.begin(), languages.end(), "und") == languages.end()) languages.push_back("und");
  return languages;
}

std::vector<Uuid16> ScopeCandidates(const ResolutionContext& context) {
  std::vector<Uuid16> scopes;
  PushUnique(&scopes, context.target_schema);
  PushUnique(&scopes, context.current_schema);
  for (const auto& scope : context.search_path) PushUnique(&scopes, scope);
  PushUnique(&scopes, context.default_root);
  PushUnique(&scopes, Uuid16{});
  return scopes;
}

ResolveOutcome Resolve(const NameFoldRule& rule, NameListCache& names, const ResolutionContext& context,
                       const ResolveRequest& request) {
  ResolveOutcome outcome;
  if (request.path.empty() || request.path.back().spelling.empty()) {
    outcome.status = ResolveStatus::invalid_request;
    return outcome;
  }
  const auto languages = LanguageCandidates(context);
  for (const auto& step : request.path) outcome.keys.push_back(rule.Key(step.spelling, step.quoted));

  std::vector<Uuid16> scopes = ScopeCandidates(context);
  bool steps_unique = true;
  // The qualifier steps: schema entries of the current scopes, per language.
  for (std::size_t i = 0; i + 1 < request.path.size(); ++i) {
    if (request.path[i].spelling.empty()) return outcome;  // not_found
    std::vector<Uuid16> next;
    std::optional<NameEntry> step_entry;
    for (const auto& language : languages) {
      for (const auto& scope : scopes) {
        for (const auto& entry : names.Page(scope, language)) {
          if (entry.object_class != "schema") continue;
          if (OwnLane(rule, entry) != outcome.keys[i]) continue;
          if (!IsNil(entry.object)) {
            PushUnique(&next, entry.object);
            if (!step_entry) step_entry = entry;
          }
        }
      }
      if (!next.empty()) break;
    }
    if (next.size() != 1) steps_unique = false;
    if (step_entry) outcome.steps.push_back(*step_entry);
    scopes = std::move(next);
    if (scopes.empty()) return outcome;  // not_found
  }

  // The leaf: per language, per scope; the first language with any match wins.
  const auto& leaf_key = outcome.keys.back();
  for (const auto& language : languages) {
    for (const auto& scope : scopes) {
      for (const auto& entry : names.Page(scope, language)) {
        if (request.accepts_class && !request.accepts_class(entry.object_class)) continue;
        if (OwnLane(rule, entry) != leaf_key) continue;
        if (std::find(outcome.objects.begin(), outcome.objects.end(), entry.object) == outcome.objects.end()) {
          if (outcome.objects.empty()) outcome.entry = entry;
          outcome.objects.push_back(entry.object);
        }
      }
    }
    if (!outcome.objects.empty()) {
      outcome.status = outcome.objects.size() == 1 ? ResolveStatus::found : ResolveStatus::ambiguous;
      outcome.steps.push_back(outcome.entry);
      outcome.steps_unique = steps_unique && outcome.objects.size() == 1;
      return outcome;
    }
  }
  return outcome;  // not_found
}

std::optional<std::string> StoredPresentedName(const ResolveOutcome& outcome) {
  if (outcome.status != ResolveStatus::found || !outcome.steps_unique || outcome.steps.empty()) return std::nullopt;
  std::vector<NameComponent> components;
  for (const auto& step : outcome.steps) {
    if (!step.was_quoted &&
        (step.raw_text.empty() || step.raw_text.find('.') != std::string::npos || step.raw_text.find('"') != std::string::npos))
      return std::nullopt;
    components.push_back({step.raw_text, step.was_quoted});
  }
  return RenderPresentedName(components);
}

bool ListedNameSource::Probe(const Uuid16& scope, std::string_view language, std::uint64_t* digest) {
  const auto language_uuid = language_of_tag_ ? language_of_tag_(language) : std::nullopt;
  if (!language_uuid) return false;
  ++pages_fetched_;
  const auto page = fetch_(scope, *language_uuid, std::string(), 1);
  if (!page.ok) return false;
  last_digest_ = page.names_digest;
  *digest = page.names_digest;
  return true;
}

std::vector<NameEntry> ListedNameSource::List(const Uuid16& scope, std::string_view language) {
  std::vector<NameEntry> out;
  if (IsNil(scope)) return out;  // the nil scope (ScopeCandidates ends with it, as the engine's does) holds no names: nobody is asked
  const auto language_uuid = language_of_tag_ ? language_of_tag_(language) : std::nullopt;
  if (!language_uuid) return out;  // a language with no constant has no names
  std::string continuation;
  for (;;) {
    ++pages_fetched_;
    const auto page = fetch_(scope, *language_uuid, continuation, page_entries_);
    if (!page.ok) {
      failed_ = true;
      refusal_code_ = page.refusal_code;
      return {};
    }
    last_digest_ = page.names_digest;
    for (const auto& listed : page.entries) {
      if (listed.is_fallback) continue;
      out.push_back({scope, listed.object, std::string(language), class_of_kind_ ? class_of_kind_(listed.object_kind) : std::string(),
                     listed.name, listed.was_quoted});
    }
    if (!page.more) break;
    continuation = page.next_continuation;
  }
  return out;
}

}  // namespace scratchbird::parsers::name_resolution
