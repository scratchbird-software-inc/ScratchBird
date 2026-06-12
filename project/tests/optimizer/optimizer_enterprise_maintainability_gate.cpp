// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: OEIC_OPTIMIZER_MAINTAINABILITY_REFACTOR

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef SCRATCHBIRD_SOURCE_ROOT
#define SCRATCHBIRD_SOURCE_ROOT ""
#endif

namespace {

std::filesystem::path SourceRoot() {
  const std::string configured = SCRATCHBIRD_SOURCE_ROOT;
  if (!configured.empty()) return configured;
  const char* from_env = std::getenv("SCRATCHBIRD_SOURCE_ROOT");
  if (from_env != nullptr && *from_env != '\0') return from_env;
  return std::filesystem::current_path();
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot read " << path << '\n';
    std::exit(1);
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::size_t LineCount(const std::string& text) {
  std::size_t lines = 0;
  for (const char ch : text) {
    if (ch == '\n') ++lines;
  }
  return lines + (text.empty() || text.back() == '\n' ? 0 : 1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

int main() {
  const auto root = SourceRoot();
  const auto optimizer_dir = root / "src" / "engine" / "optimizer";
  const auto runtime_core =
      optimizer_dir / "runtime_consumption_evidence.cpp";
  const auto runtime_benchmark =
      optimizer_dir / "runtime_consumption_benchmark_evidence.cpp";
  const auto cmake = optimizer_dir / "CMakeLists.txt";

  const std::string core = Read(runtime_core);
  const std::string benchmark = Read(runtime_benchmark);
  const std::string cmake_text = Read(cmake);

  Require(LineCount(core) <= 650,
          "runtime_consumption_evidence.cpp exceeded core classifier budget");
  Require(LineCount(benchmark) <= 1250,
          "runtime_consumption_benchmark_evidence.cpp exceeded benchmark-domain budget");
  Require(Contains(cmake_text, "runtime_consumption_benchmark_evidence.cpp"),
          "optimizer CMake target is missing benchmark evidence split unit");
  Require(Contains(core, "EvaluateRouteCompletionClaim"),
          "core runtime evidence unit lost route-completion guard");
  Require(!Contains(core, "ValidateBenchmarkMethodologyEvidence"),
          "benchmark methodology validator drifted back into core runtime evidence unit");
  Require(!Contains(core, "ValidateBestMethodBenchmarkEquivalence"),
          "reference comparison validator drifted back into core runtime evidence unit");
  Require(Contains(benchmark, "OEIC_OPTIMIZER_MAINTAINABILITY_REFACTOR"),
          "benchmark evidence split lacks OEIC maintainability search key");
  Require(Contains(benchmark, "cockroachdb") &&
              Contains(benchmark, "elasticsearch") &&
              Contains(benchmark, "postgresql"),
          "benchmark evidence split lost 24-reference route validation material");

  std::cout << "OEIC optimizer maintainability refactor gate passed\n";
  return 0;
}
