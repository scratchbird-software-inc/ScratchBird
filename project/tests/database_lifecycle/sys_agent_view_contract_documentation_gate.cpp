// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/sys_information_projection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SB_AGENT_SYS_VIEW_CONTRACT_DOC
#error "SB_AGENT_SYS_VIEW_CONTRACT_DOC must point to the durable sys-view contract doc"
#endif

namespace {

namespace info = scratchbird::engine::internal_api;

const std::vector<std::string> kDirectViews = {
    "sys.agents",
    "sys.agent_metric_dependencies",
    "sys.agent_policies",
    "sys.agent_actions",
    "sys.agent_overrides",
    "sys.agent_evidence",
    "sys.agent_audit",
    "sys.filespace_capacity_agent_state",
    "sys.page_allocation_agent_state",
    "sys.filespace_shrink_readiness",
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream in(path);
  Require(in.good(), "unable to open contract doc: " + path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string Trim(std::string value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [&](char ch) {
                return !is_space(static_cast<unsigned char>(ch));
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
                return !is_space(static_cast<unsigned char>(ch));
              }).base(),
              value.end());
  return value;
}

std::string Lower(std::string value) {
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::string StripBackticks(std::string value) {
  value = Trim(std::move(value));
  if (value.size() >= 2 && value.front() == '`' && value.back() == '`') {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::vector<std::string> SplitTableLine(std::string_view line) {
  std::vector<std::string> cells;
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t next = line.find('|', start);
    std::string_view cell = next == std::string_view::npos
                                ? line.substr(start)
                                : line.substr(start, next - start);
    cells.push_back(Trim(std::string(cell)));
    if (next == std::string_view::npos) { break; }
    start = next + 1;
  }
  if (!cells.empty() && cells.front().empty()) { cells.erase(cells.begin()); }
  if (!cells.empty() && cells.back().empty()) { cells.pop_back(); }
  return cells;
}

struct DocColumn {
  std::string logical_type;
  std::string nullability;
  std::string meaning;
  std::string visibility;
};

struct DocView {
  std::string section_text;
  std::map<std::string, DocColumn> columns;
};

std::map<std::string, DocView> ParseDoc(const std::string& text) {
  std::map<std::string, DocView> views;
  std::string current_view;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("## `", 0) == 0) {
      const std::size_t end = line.find('`', 4);
      current_view = end == std::string::npos ? std::string{} : line.substr(4, end - 4);
      if (!current_view.empty()) {
        views[current_view];
      }
      continue;
    }
    if (current_view.empty()) { continue; }
    auto& view = views[current_view];
    view.section_text += line;
    view.section_text += '\n';
    if (line.rfind("| `", 0) != 0) { continue; }
    const auto cells = SplitTableLine(line);
    if (cells.size() != 5) {
      Fail("unexpected column table shape in " + current_view + ": " + line);
    }
    const std::string column_name = StripBackticks(cells[0]);
    view.columns[column_name] = {
        StripBackticks(cells[1]),
        cells[2],
        cells[3],
        cells[4],
    };
  }
  return views;
}

void RequireAuthorityNotes(const std::string& doc) {
  const std::string lower = Lower(doc);
  Require(lower.find("projections only") != std::string::npos,
          "projection-only authority note missing");
  Require(lower.find("mga snapshot visibility") != std::string::npos,
          "MGA snapshot visibility note missing");
  Require(lower.find("engine-owned") != std::string::npos,
          "engine-owned authorization/redaction note missing");
  Require(lower.find("storage authority") != std::string::npos,
          "storage authority boundary note missing");
  Require(lower.find("transaction authority") != std::string::npos,
          "transaction authority boundary note missing");
  Require(lower.find("finality authority") != std::string::npos,
          "finality authority boundary note missing");
}

void ValidateView(const std::string& view_name, const DocView& doc_view) {
  const auto* definition = info::FindSysInformationProjectionDefinition(view_name);
  Require(definition != nullptr, view_name + " implementation definition missing");
  Require(!definition->description.empty(), view_name + " implementation description missing");
  Require(!doc_view.columns.empty(), view_name + " documentation has no columns");

  std::set<std::string> implemented_columns;
  for (const auto& column : definition->columns) {
    implemented_columns.insert(column.column_name);
    const auto found = doc_view.columns.find(column.column_name);
    Require(found != doc_view.columns.end(),
            view_name + " missing documented column " + column.column_name);
    Require(found->second.logical_type == column.logical_type,
            view_name + "." + column.column_name + " datatype mismatch: doc=" +
                found->second.logical_type + " impl=" + column.logical_type);
    Require(!found->second.nullability.empty(),
            view_name + "." + column.column_name + " nullability missing");
    Require(!found->second.meaning.empty(),
            view_name + "." + column.column_name + " meaning missing");
    Require(!found->second.visibility.empty(),
            view_name + "." + column.column_name + " redaction note missing");
    if (info::SysInformationProjectionColumnNameExposesUuid(column.column_name)) {
      const std::string visibility = Lower(found->second.visibility);
      Require(visibility.find("database-generated") != std::string::npos ||
                  visibility.find("resolver-sourced") != std::string::npos,
              view_name + "." + column.column_name + " resolver UUID note missing");
      Require(visibility.find("redacted") != std::string::npos,
              view_name + "." + column.column_name + " redaction note missing");
    }
  }

  for (const auto& [column_name, column] : doc_view.columns) {
    (void)column;
    Require(implemented_columns.count(column_name) == 1,
            view_name + " documents non-implemented column " + column_name);
  }

  const std::string section_lower = Lower(doc_view.section_text);
  Require(section_lower.find("select ") != std::string::npos,
          view_name + " SELECT example missing");
  Require(section_lower.find("from " + view_name) != std::string::npos,
          view_name + " SELECT example does not reference the view");
  Require(section_lower.find("key filters:") != std::string::npos,
          view_name + " key filter/use-case note missing");
}

}  // namespace

int main() {
  const std::string doc_path = SB_AGENT_SYS_VIEW_CONTRACT_DOC;
  const std::string execution_plan_token = std::string("docs/") + "execution-plans/";
  Require(doc_path.find(execution_plan_token) == std::string::npos,
          "contract gate must not read execution_plan documentation");
  const std::string doc = ReadTextFile(doc_path);
  RequireAuthorityNotes(doc);
  const auto parsed = ParseDoc(doc);
  for (const auto& view : kDirectViews) {
    const auto found = parsed.find(view);
    Require(found != parsed.end(), view + " section missing from contract doc");
    ValidateView(view, found->second);
  }
  return EXIT_SUCCESS;
}
