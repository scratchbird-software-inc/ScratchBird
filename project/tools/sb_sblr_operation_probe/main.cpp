// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CatalogFile {
  std::string logical_name;
  std::string path;
  bool required = true;
};

struct Args {
  std::string profile;
  std::string sblr_operation;
  std::string report;
  std::vector<CatalogFile> catalogs;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string message;
  std::string path;
};

struct Declaration {
  std::string sblr_operation;
  std::string source_file;
  int line = 0;
  std::string declaration_text;
};

std::string Trim(const std::string& value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n|`-'\"");
  if (first == std::string::npos) {
    return "";
  }
  const std::size_t last = value.find_last_not_of(" \t\r\n|`,'\"");
  return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string ToUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

bool Contains(const std::string& value, const std::string& needle) {
  return ToLower(value).find(ToLower(needle)) != std::string::npos;
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

std::uint64_t FnvaUpdate(std::uint64_t hash, const std::string& value) {
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool ReadFile(const std::string& path, std::string* content) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  *content = buffer.str();
  return true;
}

bool SplitCatalogSpec(const std::string& spec, bool required, CatalogFile* file, std::string* error) {
  const std::size_t marker = spec.find('=');
  if (marker == std::string::npos || marker == 0 || marker + 1 >= spec.size()) {
    *error = "catalog argument must use logical-name=path: " + spec;
    return false;
  }
  file->logical_name = spec.substr(0, marker);
  file->path = spec.substr(marker + 1);
  file->required = required;
  return true;
}

bool ParseArgs(int argc, char** argv, Args* args, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto require_value = [&](std::string* target) -> bool {
      if (i + 1 >= argc) {
        *error = "missing value for " + key;
        return false;
      }
      *target = argv[++i];
      return true;
    };

    if (key == "--profile") {
      if (!require_value(&args->profile)) return false;
    } else if (key == "--sblr-operation") {
      if (!require_value(&args->sblr_operation)) return false;
    } else if (key == "--report") {
      if (!require_value(&args->report)) return false;
    } else if (key == "--catalog" || key == "--optional-catalog") {
      std::string value;
      if (!require_value(&value)) return false;
      CatalogFile file;
      if (!SplitCatalogSpec(value, key == "--catalog", &file, error)) {
        return false;
      }
      args->catalogs.push_back(file);
    } else {
      *error = "unknown argument: " + key;
      return false;
    }
  }

  if (args->profile.empty() || args->sblr_operation.empty() || args->report.empty() || args->catalogs.empty()) {
    *error = "--profile, --sblr-operation, at least one --catalog, and --report are required";
    return false;
  }

  args->sblr_operation = ToUpper(args->sblr_operation);
  return true;
}

std::string ExtractValueAfterMarker(const std::string& line, const std::string& marker) {
  const std::string lower = ToLower(line);
  const std::size_t pos = lower.find(marker);
  if (pos == std::string::npos) {
    return "";
  }
  std::string value = Trim(line.substr(pos + marker.size()));
  const std::size_t terminator = value.find_first_of(" |,\t");
  if (terminator != std::string::npos) {
    value = value.substr(0, terminator);
  }
  return Trim(value);
}

std::vector<std::string> ExtractSblrTokens(const std::string& line) {
  std::vector<std::string> tokens;
  const std::vector<std::string> markers = {
      "sblr_operation:", "sblr-operation:", "sblr_operation=", "sblr-operation=",
      "sblr_opcode:", "sblr-opcode:", "sblr_opcode=", "sblr-opcode=",
      "internal_operation:", "internal-operation:", "internal_operation=", "internal-operation=",
      "logical_operation:", "logical-operation:", "logical_operation=", "logical-operation="};

  for (const auto& marker : markers) {
    const std::string value = ExtractValueAfterMarker(line, marker);
    if (!value.empty()) {
      tokens.push_back(value);
    }
  }

  std::istringstream cells(line);
  std::string cell;
  while (std::getline(cells, cell, '|')) {
    cell = Trim(cell);
    const std::string upper = ToUpper(cell);
    if ((upper.rfind("SBLR_", 0) == 0 || upper.rfind("SBOP_", 0) == 0 || upper.rfind("OP_", 0) == 0) &&
        cell.find(' ') == std::string::npos &&
        cell.find(':') == std::string::npos &&
        cell.find('=') == std::string::npos) {
      tokens.push_back(cell);
    }
  }

  return tokens;
}

bool IsRawSqlOperationName(const std::string& operation) {
  const std::string upper = ToUpper(operation);
  return Contains(upper, "RAW_SQL") ||
         Contains(upper, "EXECUTE_SQL") ||
         Contains(upper, "SQL_TEXT") ||
         Contains(upper, "SQL_ENGINE") ||
         Contains(upper, "PASS_THROUGH_SQL");
}

void ExtractDeclarations(const std::string& source_file,
                         const std::string& content,
                         std::vector<Declaration>* declarations,
                         std::vector<Diagnostic>* diagnostics) {
  std::istringstream lines(content);
  std::string line;
  int line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    for (const auto& token : ExtractSblrTokens(line)) {
      const std::string normalized = ToUpper(token);
      declarations->push_back({normalized, source_file, line_number, trimmed});
      if (IsRawSqlOperationName(normalized) || IsRawSqlOperationName(trimmed)) {
        diagnostics->push_back({"SB-SBLR-OPERATION-RAW-SQL-FORBIDDEN", "error", "SBLR operation catalog must not declare raw SQL execution as an engine operation", source_file});
      }
    }
  }
}

std::string BuildReport(const Args& args,
                        const std::vector<Diagnostic>& diagnostics,
                        const std::vector<Declaration>& matches,
                        std::uint64_t snapshot_hash) {
  const bool ok = diagnostics.empty() && matches.size() == 1;
  std::ostringstream out;
  out << "{\n";
  out << "  \"status\": \"" << (ok ? "pass" : "fail") << "\",\n";
  out << "  \"profile\": \"" << JsonEscape(args.profile) << "\",\n";
  out << "  \"sblr_operation\": \"" << JsonEscape(args.sblr_operation) << "\",\n";
  out << "  \"snapshot_hash\": \"" << snapshot_hash << "\",\n";
  out << "  \"match_count\": " << matches.size() << ",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& diagnostic = diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"severity\": \"" << JsonEscape(diagnostic.severity)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"path\": \"" << JsonEscape(diagnostic.path) << "\"}";
    if (i + 1 != diagnostics.size()) out << ",";
    out << "\n";
  }
  out << "  ],\n";
  out << "  \"matches\": [\n";
  for (std::size_t i = 0; i < matches.size(); ++i) {
    const auto& match = matches[i];
    out << "    {\"sblr_operation\": \"" << JsonEscape(match.sblr_operation)
        << "\", \"source_file\": \"" << JsonEscape(match.source_file)
        << "\", \"line\": " << match.line
        << ", \"declaration_text\": \"" << JsonEscape(match.declaration_text) << "\"}";
    if (i + 1 != matches.size()) out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  std::string error;
  if (!ParseArgs(argc, argv, &args, &error)) {
    std::cerr << error << "\n";
    std::cerr << "usage: sb_sblr_operation_probe --profile <profile> --sblr-operation <operation> --catalog <logical-name=path> --report <path>\n";
    return 5;
  }

  std::vector<Diagnostic> diagnostics;
  std::vector<Declaration> declarations;
  std::uint64_t hash = 1469598103934665603ull;
  hash = FnvaUpdate(hash, args.profile);
  hash = FnvaUpdate(hash, args.sblr_operation);

  if (IsRawSqlOperationName(args.sblr_operation)) {
    diagnostics.push_back({"SB-SBLR-OPERATION-RAW-SQL-FORBIDDEN", "error", "requested SBLR operation is raw SQL execution, which is forbidden by the engine design", ""});
  }

  for (const auto& catalog : args.catalogs) {
    hash = FnvaUpdate(hash, catalog.logical_name);
    hash = FnvaUpdate(hash, catalog.path);

    std::string content;
    if (!ReadFile(catalog.path, &content)) {
      diagnostics.push_back({catalog.required ? "SB-SBLR-OPERATION-CATALOG-MISSING-FILE" : "SB-SBLR-OPERATION-CATALOG-OPTIONAL-FILE-MISSING",
                             catalog.required ? "error" : "warning",
                             catalog.required ? "required SBLR operation catalog could not be read" : "optional SBLR operation catalog could not be read",
                             catalog.path});
      continue;
    }

    hash = FnvaUpdate(hash, content);
    ExtractDeclarations(catalog.path, content, &declarations, &diagnostics);
  }

  std::vector<Declaration> matches;
  for (const auto& declaration : declarations) {
    if (declaration.sblr_operation == args.sblr_operation) {
      matches.push_back(declaration);
    }
  }

  if (matches.empty()) {
    diagnostics.push_back({"SB-SBLR-OPERATION-NOT-FOUND", "error", "SBLR operation is not declared in the supplied catalogs", ""});
  } else if (matches.size() > 1) {
    diagnostics.push_back({"SB-SBLR-OPERATION-AMBIGUOUS", "error", "SBLR operation has more than one declaration in the supplied catalogs", ""});
  }

  std::ofstream out(args.report);
  if (!out) {
    std::cerr << "failed to write report: " << args.report << "\n";
    return 4;
  }
  out << BuildReport(args, diagnostics, matches, hash);

  return diagnostics.empty() && matches.size() == 1 ? 0 : 1;
}
