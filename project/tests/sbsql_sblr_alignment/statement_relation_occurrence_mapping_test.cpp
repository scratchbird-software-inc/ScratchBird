// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_relation_occurrence_mapping.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace { long fail_after = -1; unsigned checks = 0; }
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using Mapping = scratchbird::server_engine_bridge::StatementRelationOccurrenceMappingV1;
using Projection = scratchbird::engine::sblr::SblrLiteralPersistedDescriptorMappingV1;
using Uuid = scratchbird::engine::internal_api::EngineUuid;
using scratchbird::engine::PublishStatementRelationOccurrenceMappingsV1;
static_assert(std::is_same_v<decltype(Mapping::persisted_descriptor_uuid), Uuid>);
static_assert(sizeof(Uuid) == 16);

void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
Uuid Id(unsigned n) {
  return Uuid{{0, 1, 2, 3, 4, 5, 0x76, 7, 0x88, 9, 10, 11, 12, 13,
               static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)}};
}
bool Same(const std::vector<Mapping>& a, const std::vector<Mapping>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].occurrence_id != b[i].occurrence_id ||
        a[i].persisted_descriptor_uuid != b[i].persisted_descriptor_uuid ||
        a[i].persisted_descriptor_generation != b[i].persisted_descriptor_generation)
      return false;
  }
  return true;
}
bool Same(const std::vector<Projection>& a, const std::vector<Projection>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].occurrence_id != b[i].occurrence_id ||
        a[i].persisted_descriptor_uuid != b[i].persisted_descriptor_uuid ||
        a[i].persisted_descriptor_generation != b[i].persisted_descriptor_generation)
      return false;
  }
  return true;
}
void Exact(const std::vector<Mapping>& expected, const std::vector<Mapping>& view,
           const std::vector<Projection>& executor) {
  Check(Same(expected, view), "receipt view changed mapping fields");
  Check(expected.size() == executor.size(), "executor mapping count differs");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    Check(expected[i].occurrence_id == executor[i].occurrence_id,
          "occurrence changed");
    Check(expected[i].persisted_descriptor_generation == executor[i].persisted_descriptor_generation,
          "generation changed");
    for (unsigned byte = 0; byte < 16; ++byte) {
      Check(expected[i].persisted_descriptor_uuid.bytes[byte] == executor[i].persisted_descriptor_uuid[byte],
            "descriptor bytes changed");
    }
  }
}
void Invalid(const std::vector<Mapping>& input) {
  std::vector<Mapping> view{{1000, Id(9), 7}};
  std::vector<Projection> executor{{1000, Id(9).bytes, 7}};
  const auto old_view = view;
  const auto old_executor = executor;
  // Validation must precede even the first staging allocation.
  fail_after = 0;
  bool accepted = false;
  try { accepted = PublishStatementRelationOccurrenceMappingsV1(input, view, executor); }
  catch (...) { fail_after = -1; throw; }
  fail_after = -1;
  Check(!accepted, "invalid mapping accepted");
  Check(Same(view, old_view) && Same(executor, old_executor),
        "invalid mapping changed receipt projections");
}
void Cases() {
  const std::vector<Mapping> base{{1, Id(1), 1}, {9, Id(1), 42}};
  std::vector<Mapping> view;
  std::vector<Projection> executor;
  Check(PublishStatementRelationOccurrenceMappingsV1(base, view, executor), "valid mapping refused");
  Exact(base, view, executor);
  // Multiple occurrences of the same descriptor are legal.
  Check(PublishStatementRelationOccurrenceMappingsV1(view, view, executor), "aliased input refused");
  Exact(base, view, executor);
  auto max = base;
  max.back().occurrence_id = std::numeric_limits<std::uint64_t>::max();
  max.back().persisted_descriptor_generation = std::numeric_limits<std::uint64_t>::max();
  Check(PublishStatementRelationOccurrenceMappingsV1(max, view, executor), "maximum u64 refused");
  Exact(max, view, executor);
  Check(PublishStatementRelationOccurrenceMappingsV1({}, view, executor), "empty mapping refused");
  Check(view.empty() && executor.empty(), "empty mapping projections differ");

  for (unsigned field = 0; field < 5; ++field) {
    auto input = base;
    if (field == 0) input.front().occurrence_id = 0;
    if (field == 1) input.back().occurrence_id = 1;
    if (field == 2) input.back().occurrence_id = 0;
    if (field == 3) input.back().persisted_descriptor_generation = 0;
    if (field == 4) input.back().persisted_descriptor_uuid = {};
    Invalid(input);
  }
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto input = base;
    input.back().persisted_descriptor_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    Invalid(input);
  }
  for (unsigned variant : {0x00u, 0x40u, 0xc0u}) {
    auto input = base;
    input.back().persisted_descriptor_uuid.bytes[8] = static_cast<std::uint8_t>(variant);
    Invalid(input);
  }
  // Every non-version/variant identity bit is retained, including zero bytes.
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((byte == 6 && bit >= 4) || (byte == 8 && bit >= 6)) continue;
      auto input = base;
      input.back().persisted_descriptor_uuid.bytes[byte] ^= (1u << bit);
      Check(PublishStatementRelationOccurrenceMappingsV1(input, view, executor), "valid bit change refused");
      Exact(input, view, executor);
    }
  }
  unsigned failures = 0;
  for (const unsigned count : {1u, 2u, 64u}) {
    std::vector<Mapping> input;
    for (unsigned i = 0; i < count; ++i) input.push_back({i + 1, Id(i), i + 10});
    for (long fault = 0; fault < 3; ++fault) {
      view = {{999, Id(10), 12}};
      executor = {{999, Id(10).bytes, 12}};
      const auto old_view = view;
      const auto old_executor = executor;
      fail_after = fault;
      bool failed = false;
      try {
        if (!PublishStatementRelationOccurrenceMappingsV1(input, view, executor)) {
          fail_after = -1;
          throw std::runtime_error("valid allocation fixture rejected");
        }
      } catch (const std::bad_alloc&) { failed = true; }
      fail_after = -1;
      if (fault < 2) {
        ++failures;
        Check(failed, "staging allocation not faulted");
        Check(Same(view, old_view) && Same(executor, old_executor),
              "allocation failure partially published mapping");
        Check(PublishStatementRelationOccurrenceMappingsV1(input, view, executor), "retry failed");
      } else Check(!failed, "allocation after staging commit");
      Exact(input, view, executor);
    }
  }
  std::cout << "PASS checks=" << checks << " allocation_faults=" << failures << '\n';
}
}  // namespace

int main() {
  try { Cases(); return 0; }
  catch (const std::exception& e) { fail_after = -1; std::cerr << e.what() << '\n'; return 1; }
}
