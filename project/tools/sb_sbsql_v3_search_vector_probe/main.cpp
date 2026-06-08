// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/search_api.hpp"
#include "nosql/vector_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else {
      return false;
    }
  }
  return !args->path.empty();
}

EngineRequestContext Context(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-search-vector-probe";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001201";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001202";
  return context;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_search_vector_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto context = Context(args);

  EngineSearchQueryRequest search;
  search.context = context;
  search.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001211";
  search.descriptors.push_back({{"00000000-0000-7000-8000-000000001212"}, "search_descriptor", "full_text", "language=en;tokenizer=default"});
  search.predicate.predicate_kind = "search_text";
  search.predicate.canonical_predicate_envelope = "contains(customer, 'gold')";
  const auto search_result = EngineSearchQuery(search);

  EngineVectorSearchRequest vector;
  vector.context = context;
  vector.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001221";
  vector.descriptors.push_back({{"00000000-0000-7000-8000-000000001222"}, "vector_descriptor", "vector<float32,3>", "metric=cosine;dimensions=3"});
  vector.option_envelopes.push_back("approximate:true");
  const auto vector_result = EngineVectorSearch(vector);

  EngineSearchQueryRequest cluster_search;
  cluster_search.context = context;
  cluster_search.option_envelopes.push_back("distributed_search:true");
  const auto cluster_search_result = EngineSearchQuery(cluster_search);

  const bool search_ok = search_result.ok &&
                         !search_result.result_shape.rows.empty() &&
                         HasEvidence(search_result, "nosql_surface", "search") &&
                         HasEvidence(search_result, "nosql_behavior", "specialized_descriptor_fallback");
  const bool vector_ok = vector_result.ok &&
                         !vector_result.result_shape.rows.empty() &&
                         HasEvidence(vector_result, "nosql_surface", "vector") &&
                         HasEvidence(vector_result, "vector_search", "exact_fallback_available");
  const bool cluster_denied = !cluster_search_result.ok && cluster_search_result.cluster_authority_required;
  const bool ok = search_ok && vector_ok && cluster_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("search_ok", search_ok, true);
  PrintBool("vector_ok", vector_ok, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
