// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_registry.hpp"
#include "registry/function_seed_registry.hpp"
#include "metadata/function_hardening.hpp"
#include "metadata/function_parser_projection.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <type_traits>

namespace f = scratchbird::engine::functions;
namespace {
std::atomic<long> fail_after{-1};
unsigned checks = 0, failures = 0, allocation_faults = 0;
}
void* operator new(std::size_t n) {
  auto remaining = fail_after.load();
  if (remaining >= 0) {
    if (!remaining) throw std::bad_alloc();
    --fail_after;
  }
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok && ++failures < 20) std::cerr << "FAIL " << why << '\n';
}
constexpr f::FunctionUuid Base() {
  return {{0x01, 0x9f, 0x11, 0x22, 0x33, 0x44, 0x75, 0x66,
           0x87, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee}};
}
f::FunctionRegistryEntry Entry(f::FunctionUuid id, std::string name) {
  f::FunctionRegistryEntry entry;
  entry.function_uuid = id;
  entry.function_id = std::move(name);
  entry.family = "data.scalar";
  entry.short_name = "lower";
  entry.implementation_state = f::FunctionImplementationState::implemented_behavior;
  entry.owner_source = "component test input, not execution evidence";
  entry.owner_test = "function_registry_binary_test.cpp";
  return entry;
}
void Admission() {
  f::FunctionRegistry registry;
  Check(registry.empty(), "new registry not empty");
  std::string error;
  Check(!registry.Register(Entry({}, "nil"), &error) && !error.empty(), "nil system identity admitted");
  Check(!registry.Register(Entry(Base(), ""), &error), "empty symbol admitted");
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto id = Base();
    id.bytes[6] = static_cast<unsigned char>((version << 4) | 5);
    Check(!registry.Register(Entry(id, "version"), &error), "non-v7 system function identity admitted");
    Check(registry.LookupByUuid(id) == nullptr, "refused identity published");
  }
  for (unsigned variant = 0; variant < 256; ++variant) {
    if ((variant & 0xc0) == 0x80) continue;
    auto id = Base();
    id.bytes[8] = static_cast<unsigned char>(variant);
    Check(!registry.Register(Entry(id, "variant"), &error), "invalid UUID variant admitted");
  }
  Check(registry.empty(), "refused registrations mutated registry");
  Check(registry.Register(Entry(Base(), "data.scalar.lower")), "valid binary registration failed");
  const auto* original = registry.LookupByUuid(Base());
  Check(original && original == registry.Lookup("data.scalar.lower"), "lookup routes disagree");
  auto other = Base();
  ++other.bytes[15];
  Check(!registry.Register(Entry(Base(), "different symbol"), &error), "duplicate UUID admitted");
  Check(!registry.Register(Entry(other, "data.scalar.lower"), &error), "duplicate symbol admitted");
  Check(!registry.Lookup("different symbol") && !registry.LookupByUuid(other), "duplicate published a partial entry");
  Check(registry.Entries().size() == 1 && registry.LookupByUuid(Base()) == original, "duplicate changed original entry");
  Check(f::ValidateFunctionRegistryForClosure(registry).empty(), "binary identity rejected by closure validation");
  auto visible = f::BuildFunctionCatalogExportRow(*original, true);
  auto hidden = f::BuildFunctionCatalogExportRow(*original, false);
  Check(visible.function_uuid == Base() && !visible.metadata_redacted, "catalog export lost binary UUID");
  Check(hidden.function_uuid.is_nil() && hidden.metadata_redacted, "catalog redaction exposed identity");
  for (bool metadata_visible : {false, true}) {
    f::FunctionParserProjectionRequest request;
    request.parser_profile = "sbsql";
    request.metadata_visible = metadata_visible;
    request.include_disabled = true;
    auto rows = f::BuildFunctionParserProjection(registry, request);
    unsigned matches = 0;
    for (const auto& row : rows) {
      Check(!row.parser_has_authority, "metadata projection granted engine authority");
      if (row.canonical_function_id == "data.scalar.lower") {
        ++matches;
        Check(row.function_uuid == (metadata_visible ? Base() : f::FunctionUuid{}), "parser projection changed UUID or leaked redacted identity");
        Check(row.metadata_redacted != metadata_visible && row.parser_may_submit_sblr, "projection flags disagree with resolved metadata");
      } else {
        Check(row.function_uuid.is_nil() && !row.parser_may_submit_sblr, "unresolved metadata invented identity");
      }
    }
    Check(matches > 0, "test did not exercise resolved parser metadata");
  }
}
void AllBytes() {
  std::vector<f::FunctionUuid> identities{Base()};
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned value = 0; value < 256; ++value) {
      if (value == Base().bytes[byte]) continue;
      if (byte == 6 && (value & 0xf0) != 0x70) continue;
      if (byte == 8 && (value & 0xc0) != 0x80) continue;
      auto id = Base();
      id.bytes[byte] = static_cast<unsigned char>(value);
      identities.push_back(id);
    }
  }
  f::FunctionRegistry registry;
  std::vector<const f::FunctionRegistryEntry*> pointers;
  for (std::size_t i = 0; i < identities.size(); ++i) {
    // Long names make accidental text construction visible to the no-allocation gate.
    Check(registry.Register(Entry(identities[i], "binary-function-identity-test-symbol-" + std::to_string(i))), "full-width identities aliased");
    pointers.push_back(registry.LookupByUuid(identities[i]));
  }
  for (std::size_t i = 0; i < identities.size(); ++i) {
    Check(registry.LookupByUuid(identities[i]) == pointers[i], "insertion invalidated borrowed registry entry");
    Check(pointers[i] && pointers[i]->function_uuid == identities[i], "binary key selected a different entry");
  }
  auto sorted = identities;
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
    return std::memcmp(a.bytes.data(), b.bytes.data(), 16) < 0;
  });
  auto entries = registry.Entries();
  Check(entries.size() == sorted.size(), "enumeration lost identities");
  for (std::size_t i = 0; i < std::min(entries.size(), sorted.size()); ++i)
    Check(entries[i].function_uuid == sorted[i], "binary registry ordering differs from independent byte oracle");
  bool allocation_free = true;
  fail_after = 0;
  try {
    for (std::size_t i = 0; i < identities.size(); ++i)
      if (registry.LookupByUuid(identities[i]) != pointers[i]) allocation_free = false;
    if (registry.LookupByUuid({}) != nullptr) allocation_free = false;
  } catch (const std::bad_alloc&) { allocation_free = false; }
  fail_after = -1;
  Check(allocation_free, "binary lookup allocated or returned the wrong entry");
  // Publication is single-owner. Only immutable reads are exercised concurrently.
  std::atomic<bool> stable{true};
  std::vector<std::thread> readers;
  for (unsigned t = 0; t < 4; ++t) readers.emplace_back([&] {
    for (unsigned repeat = 0; repeat < 4; ++repeat)
      for (std::size_t i = 0; i < identities.size(); ++i)
        if (registry.LookupByUuid(identities[i]) != pointers[i]) stable = false;
  });
  for (auto& reader : readers) reader.join();
  Check(stable, "immutable concurrent lookup changed identities");
  auto copied = registry;
  for (auto id : identities) {
    auto* copy = copied.LookupByUuid(id);
    Check(copy && copy->function_uuid == id && copy != registry.LookupByUuid(id), "registry copy retained foreign entry pointers");
  }
  auto moved = std::move(copied);
  for (auto id : identities) Check(moved.LookupByUuid(id) != nullptr, "registry move lost UUID index");
}
void PublicationFailures() {
  auto candidate = Base();
  ++candidate.bytes[15];
  bool completed = false;
  for (long failure = 0; failure < 1024; ++failure) {
    f::FunctionRegistry registry;
    Check(registry.Register(Entry(Base(), "existing-symbol-with-nontrivial-length")), "allocation fixture setup failed");
    const auto* original = registry.LookupByUuid(Base());
    auto input = Entry(candidate, "candidate-symbol-with-nontrivial-length");
    bool threw = false, registered = false;
    fail_after = failure;
    try { registered = registry.Register(std::move(input)); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Check(registry.LookupByUuid(Base()) == original, "failed publication changed existing entry address");
    Check(registry.Lookup("existing-symbol-with-nontrivial-length") == original, "failed publication damaged original symbol route");
    if (threw) {
      ++allocation_faults;
      Check(!registry.LookupByUuid(candidate), "allocation failure left UUID-only entry");
      Check(!registry.Lookup("candidate-symbol-with-nontrivial-length"), "allocation failure left symbol-only entry");
      Check(registry.Entries().size() == 1, "allocation failure changed visible registry size");
      Check(registry.Register(Entry(candidate, "candidate-symbol-with-nontrivial-length")), "failed publication poisoned retry");
    } else {
      Check(registered && registry.Entries().size() == 2, "successful publication incomplete");
      Check(registry.LookupByUuid(candidate) == registry.Lookup("candidate-symbol-with-nontrivial-length"), "successful publication routes diverged");
      completed = true;
      break;
    }
  }
  Check(completed && allocation_faults > 3, "allocation sweep did not reach full publication");
}
void ProductionSeeds() {
  const auto package = f::BuildStandardFunctionSeedPackage();
  Check(!package.registry.empty() && !package.catalog_registry.empty(), "standard seed package empty");
  // Frozen source inventory regression, not proof of function execution.
  Check(package.registry.Entries().size() == 986 &&
        package.catalog_registry.Entries().size() == 114 && package.name_rows.size() == 6257,
        "binary migration lost standard seed rows");
  for (const auto& entry : package.registry.Entries()) {
    const auto* by_uuid = package.registry.LookupByUuid(entry.function_uuid);
    Check(by_uuid && by_uuid == package.registry.Lookup(entry.function_id), "standard seed lookup routes disagree");
    Check(scratchbird::core::uuid::IsEngineIdentityUuid(entry.function_uuid), "standard seed is not a system UUIDv7");
  }
  for (const auto& row : package.name_rows) {
    const auto* entry = package.catalog_registry.LookupByUuid(row.function_uuid);
    Check(entry && entry->function_id == row.canonical_function_id, "name seed points to missing or wrong binary catalog function");
    const auto* runtime = package.registry.LookupByUuid(row.function_uuid);
    Check(runtime && runtime->function_id == row.canonical_function_id, "name seed and runtime UUID bindings disagree");
  }
  // These independently fixed bytes preserve an existing canonical seed;
  // constructor and name projection must not regenerate its identity.
  constexpr f::FunctionUuid abs_id{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x76, 0x71,
                                   0x9f, 0x1b, 0x53, 0x50, 0xd1, 0x34, 0xab, 0x0f}};
  const auto* abs = package.registry.LookupByUuid(abs_id);
  Check(abs && abs->function_id == "sb.scalar.abs", "canonical abs seed identity changed");
  std::cout << package.registry.Entries().size() << " runtime seeds, "
            << package.catalog_registry.Entries().size() << " catalog seeds, "
            << package.name_rows.size() << " name seeds\n";
}
}
int main() {
  static_assert(sizeof(f::FunctionUuid) == 16);
  static_assert(std::is_same_v<decltype(f::FunctionRegistryEntry{}.function_uuid), f::FunctionUuid>);
  static_assert(std::is_same_v<decltype(f::FunctionCatalogExportRow{}.function_uuid), f::FunctionUuid>);
  static_assert(std::is_same_v<decltype(f::FunctionParserProjectionRow{}.function_uuid), f::FunctionUuid>);
  static_assert(std::is_same_v<decltype(f::FunctionNameSeedRow{}.function_uuid), f::FunctionUuid>);
  static_assert(!std::is_convertible_v<std::string, f::FunctionUuid>);
  Admission();
  AllBytes();
  PublicationFailures();
  ProductionSeeds();
  std::cout << checks << " checks, " << allocation_faults << " allocation faults, " << failures << " failures\n";
  return failures ? 1 : 0;
}
