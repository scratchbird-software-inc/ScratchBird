// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_registry.hpp"
#include "registry/function_seed_registry.hpp"
#include "metadata/function_hardening.hpp"
#include "metadata/function_parser_projection.hpp"
#include "common/function_result_helpers.hpp"
#include "sblr/sblr_aggregate_window_runtime.hpp"
#include "sblr/sblr_function_diagnostic.hpp"
#include "internal_api/api_types.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <type_traits>

namespace f = scratchbird::engine::functions;
namespace s = scratchbird::engine::sblr;
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
void CallBinding() {
  f::FunctionRegistry registry;
  auto entry = Entry(Base(), "engine-owned-canonical-symbol-with-long-name");
  entry.family = "engine-owned-package-with-long-name";
  Check(registry.Register(entry), "binding fixture registration failed");
  f::FunctionCallContext input;
  input.function_uuid = Base();
  input.function_id = "untrusted-symbol-must-not-select-a-function";
  input.package_name = "untrusted-package-must-not-select-a-handler";
  input.sblr_context.session_uuid = Base();
  input.implementation_state = f::FunctionImplementationState::policy_blocked;
  input.package_state = f::FunctionPackageState::optional;
  input.security_allowed = false;
  input.policy_allowed = false;
  input.dependency_available = false;
  bool completed = false;
  unsigned faults = 0;
  for (long fault = 0; fault < 32; ++fault) {
    auto context = input;
    bool threw = false;
    const f::FunctionRegistryEntry* selected = nullptr;
    fail_after = fault;
    try { selected = registry.BindCallContext(context); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Check(context.function_uuid == Base() && context.sblr_context.session_uuid == Base(), "binding changed binary request identity");
    Check(!context.security_allowed && !context.policy_allowed && !context.dependency_available, "binding granted execution gates");
    if (threw) {
      ++faults;
      Check(context.function_id == input.function_id && context.package_name == input.package_name &&
            context.implementation_state == input.implementation_state && context.package_state == input.package_state,
            "binding allocation failure partially replaced authority metadata");
    } else {
      Check(selected == registry.LookupByUuid(Base()) && context.function_id == entry.function_id &&
            context.package_name == entry.family && context.implementation_state == entry.implementation_state &&
            context.package_state == entry.package_state, "binary UUID did not select canonical dispatch metadata");
      completed = true;
      break;
    }
  }
  allocation_faults += faults;
  Check(completed && faults == 2, "binding did not exercise both metadata allocations");
  auto absent = input;
  absent.function_id = entry.function_id;
  absent.function_uuid = {};
  fail_after = 0;
  bool rejected = false;
  try { rejected = registry.BindCallContext(absent) == nullptr; }
  catch (const std::bad_alloc&) {}
  fail_after = -1;
  Check(rejected && absent.function_uuid.is_nil() && absent.package_name == input.package_name,
        "text symbol substituted for missing binary UUID or allocated on missing lookup");
  absent.function_uuid = Base();
  ++absent.function_uuid.bytes[0];
  Check(!registry.BindCallContext(absent), "text symbol overrode unknown binary UUID");
}
void BinaryDiagnostics() {
  s::SblrExecutionContext context;
  std::vector<std::pair<const char*, s::SblrUuid*>> fields = {
      {"cluster_uuid", &context.cluster_uuid}, {"node_uuid", &context.node_uuid},
      {"database_uuid", &context.database_uuid}, {"transaction_uuid", &context.transaction_uuid},
      {"statement_uuid", &context.statement_uuid}, {"user_uuid", &context.user_uuid},
      {"parser_profile_uuid", &context.parser_profile_uuid}, {"security_snapshot_uuid", &context.security_snapshot_uuid}};
  unsigned ordinal = 0;
  for (auto [name, id] : fields) { (void)name; *id = Base(); id->bytes[15] = static_cast<unsigned char>(ordinal++); }
  auto diagnostic = s::MakeSblrRefusalDiagnostic("SB_DIAG_FUNCTION_INVALID_INPUT", context, "invalid input");
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(diagnostic.occurrence_uuid), "diagnostic emission did not issue a binary v7 occurrence");
  auto copied = diagnostic;
  Check(copied.occurrence_uuid == diagnostic.occurrence_uuid, "diagnostic copy replaced source occurrence");
  auto second = s::MakeSblrRefusalDiagnostic("SB_DIAG_FUNCTION_INVALID_INPUT", context, "same code and context");
  Check(second.occurrence_uuid != diagnostic.occurrence_uuid, "distinct diagnostic emissions reused an occurrence");
  for (const auto& [name, id] : fields) {
    unsigned matches = 0;
    for (const auto& field : diagnostic.fields) if (field.key == name) {
      ++matches;
      const auto* value = std::get_if<s::SblrUuid>(&field.value);
      Check(value && *value == *id, "diagnostic lost or rendered a binary context identity");
    }
    Check(matches == 1, "diagnostic context identity missing or duplicated");
  }
  Check(s::ValidateDiagnosticCompleteness(diagnostic, nullptr), "typed diagnostic rejected without detail sink");
  std::vector<std::string> missing{"retained caller detail"};
  Check(s::ValidateDiagnosticCompleteness(diagnostic, &missing) && missing.size() == 1,
        "valid diagnostic damaged caller detail list");
  for (const char* key : {"database_uuid", "statement_uuid", "user_uuid", "security_snapshot_uuid"}) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto broken = diagnostic;
      const auto where = std::find_if(broken.fields.begin(), broken.fields.end(), [&](const auto& field) { return field.key == key; });
      if (mode == 0) broken.fields.erase(where);
      else if (mode == 1) where->value = std::string("019f1122-3344-7566-8788-99aabbccddee");
      else broken.fields.push_back(*where);
      Check(!s::ValidateDiagnosticCompleteness(broken, nullptr), "missing/textual/duplicate identity falsely validated without detail sink");
      std::vector<std::string> details;
      Check(!s::ValidateDiagnosticCompleteness(broken, &details) && details.size() == 1 && details.front() == key,
            "diagnostic identity failure not reported consistently");
    }
  }
  auto no_context = s::MakeSblrRefusalDiagnostic("SB_DIAG_FUNCTION_INVALID_INPUT", {}, "before context");
  Check(s::ValidateDiagnosticCompleteness(no_context, nullptr), "explicit binary nil context rejected");
  Check(!s::ValidateDiagnosticCompleteness({}, nullptr), "empty diagnostic falsely validated without detail sink");
  auto no_code = diagnostic;
  no_code.diagnostic_id.clear();
  Check(!s::ValidateDiagnosticCompleteness(no_code, nullptr), "missing diagnostic code validated");
  no_code = diagnostic;
  no_code.message_key.clear();
  Check(!s::ValidateDiagnosticCompleteness(no_code, nullptr), "missing message key validated");
  no_code = diagnostic;
  no_code.occurrence_uuid = {};
  Check(!s::ValidateDiagnosticCompleteness(no_code, nullptr), "missing diagnostic occurrence validated");
  no_code.occurrence_uuid = Base();
  no_code.occurrence_uuid.bytes[6] = 0x45;
  Check(!s::ValidateDiagnosticCompleteness(no_code, nullptr), "non-v7 diagnostic occurrence validated");
  fail_after = 0;
  bool no_allocation = false;
  try { no_allocation = s::ValidateDiagnosticCompleteness(diagnostic, nullptr); }
  catch (const std::bad_alloc&) {}
  fail_after = -1;
  Check(no_allocation, "sink-free completeness validation allocated");
  f::FunctionCallRequest request;
  request.context.sblr_context = context;
  request.context.function_uuid = Base();
  request.context.function_id = "function-under-test";
  const auto result = f::RefuseFunctionConversionInput(request, "bad-number", "invalid conversion");
  Check(!result.result.ok() && result.result.diagnostics.size() == 1, "conversion refusal not preserved");
  if (!result.result.diagnostics.empty()) {
    bool saw_function = false, saw_input = false;
    for (const auto& field : result.result.diagnostics.front().fields) {
      if (field.key == "function_uuid") {
        const auto* value = std::get_if<s::SblrUuid>(&field.value);
        saw_function = value && *value == Base();
      }
      if (field.key == "conversion_input_text") {
        const auto* value = std::get_if<std::string>(&field.value);
        saw_input = value && *value == "bad-number";
      }
    }
    Check(saw_function && saw_input, "binary identity and public conversion text lost their distinct types");
    const auto& source = result.result.diagnostics.front();
    const auto projected = s::FunctionDiagnosticToApi(source);
    Check(projected.occurrence_uuid == source.occurrence_uuid.bytes && projected.code == source.diagnostic_id &&
          projected.fields.size() == 1 && projected.fields.front().key == "conversion_input_text" &&
          projected.fields.front().value == "bad-number", "actual API bridge replaced source occurrence or lost safe conversion field");
    for (unsigned malformed = 0; malformed < 6; ++malformed) {
      auto changed = source;
      auto parameter = std::find_if(changed.fields.begin(), changed.fields.end(), [](const auto& field) { return field.key == "conversion_input_text"; });
      if (malformed == 0) changed.diagnostic_id = "SB_DIAG_EXECUTE_FUNCTION_REFUSED";
      if (malformed == 1) parameter->value = Base();
      if (malformed == 2) parameter->value = std::string(1025, 'x');
      if (malformed == 3) parameter->value = std::string("hidden\0payload", 14);
      if (malformed == 4) changed.fields.push_back(*parameter);
      if (malformed == 5) parameter->value = std::string{};
      const auto filtered = s::FunctionDiagnosticToApi(changed);
      Check(filtered.fields.empty() && filtered.occurrence_uuid == changed.occurrence_uuid.bytes && filtered.error,
            "API bridge disclosed undeclared/malformed/ambiguous private parameter or changed occurrence");
    }
  }
  s::SblrFrameStack stack;
  s::SblrFrame frame;
  frame.frame_uuid = Base();
  frame.routine_object_uuid = context.user_uuid;
  frame.package_object_uuid = context.database_uuid;
  Check(s::PushSblrFrame(&stack, frame, nullptr) && stack.frames.back().frame_uuid == Base() &&
        stack.frames.back().routine_object_uuid == context.user_uuid, "frame stack lost binary identities");
  Check(s::PopSblrFrame(&stack, nullptr) && stack.frames.empty(), "binary frame lifecycle did not finish");
}
void BinaryAggregate() {
  constexpr s::SblrUuid sum_uuid{{0x01,0x9d,0xe5,0xfc,0x24,0x00,0x72,0xe4,0x85,0x49,0x82,0xb2,0xee,0xf5,0xa7,0x77}};
  s::SblrExecutionContext context;
  context.database_uuid = Base();
  s::SblrAggregateWindowState state;
  auto initialize = s::InitializeSblrAggregateState("untrusted-label-not-authority", sum_uuid, "int64", context, &state);
  Check(initialize.ok() && state.function_uuid == sum_uuid && state.function_id == "sb.aggregate.sum",
        "aggregate initializer did not bind through binary UUID");
  for (auto number : {2, 3}) {
    s::SblrAggregateUpdateRequest update;
    update.context = context;
    update.values.push_back(f::MakeInt64Value("int64", number));
    Check(s::UpdateSblrAggregateState(&state, update).ok(), "binary-bound aggregate update failed");
  }
  s::SblrAggregateFinalizeRequest finalize;
  finalize.context = context;
  auto result = s::FinalizeSblrAggregateState(state, finalize);
  Check(result.ok() && result.scalar_values.size() == 1 && result.scalar_values.front().descriptor_id == "int64" &&
        result.scalar_values.front().payload_kind == s::SblrValuePayloadKind::high_precision_numeric_text &&
        result.scalar_values.front().encoded_value == "5", "binary-bound sum did not execute and publish five");
  const auto original = state;
  auto invalid_uuid = sum_uuid;
  ++invalid_uuid.bytes[0];
  Check(!s::InitializeSblrAggregateState("sb.aggregate.sum", invalid_uuid, "int64", context, &state).ok() &&
        state.function_uuid == original.function_uuid && state.numeric_sum == original.numeric_sum && state.input_count == original.input_count,
        "text aggregate name rescued unknown UUID or failure changed old state");
  s::SblrAggregateUpdateRequest update;
  update.context = context;
  update.values.push_back(f::MakeInt64Value("int64", 7));
  for (unsigned corruption = 0; corruption < 3; ++corruption) {
    auto bad = original;
    if (corruption == 0) bad.function_uuid = invalid_uuid;
    if (corruption == 1) bad.function_id = "sb.aggregate.avg";
    if (corruption == 2) bad.aggregate_kind = s::SblrAggregateFunctionKind::avg;
    Check(!s::UpdateSblrAggregateState(&bad, update).ok() && bad.numeric_sum == original.numeric_sum && bad.input_count == original.input_count,
          "aggregate update used a cross-bound state");
    Check(!s::FinalizeSblrAggregateState(bad, finalize).ok(), "aggregate finalize published a cross-bound state");
    auto target = original;
    Check(!s::MergeSblrAggregateState(&target, bad, context).ok() && target.numeric_sum == original.numeric_sum &&
          target.input_count == original.input_count, "aggregate merge consumed a cross-bound source");
    Check(!s::MergeSblrAggregateState(&bad, original, context).ok(), "aggregate merge accepted a cross-bound target");
  }
  auto different_descriptor = original;
  different_descriptor.result_descriptor_id = "real64";
  Check(!s::MergeSblrAggregateState(&state, different_descriptor, context).ok() && state.numeric_sum == 5,
        "aggregate merge mixed incompatible result representations");
  auto source = original;
  Check(s::MergeSblrAggregateState(&state, source, context).ok(), "matching binary aggregate states did not merge");
  result = s::FinalizeSblrAggregateState(state, finalize);
  Check(result.ok() && result.scalar_values.size() == 1 && result.scalar_values.front().descriptor_id == "int64" &&
        result.scalar_values.front().encoded_value == "10",
        "binary aggregate merge did not publish ten");
  unsigned faults = 0;
  bool completed = false;
  for (long fault = 0; fault < 32; ++fault) {
    auto target = original;
    bool threw = false, ok = false;
    fail_after = fault;
    try { ok = s::InitializeSblrAggregateState("ignored-symbol", sum_uuid, "int64", context, &target).ok(); }
    catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    if (threw) {
      ++faults;
      Check(target.function_uuid == original.function_uuid && target.function_id == original.function_id &&
            target.numeric_sum == original.numeric_sum && target.input_count == original.input_count && target.initialized,
            "aggregate reinitialization allocation failure destroyed old state");
    } else {
      Check(ok && target.function_uuid == sum_uuid && target.numeric_sum == 0 && target.input_count == 0,
            "aggregate reinitialization did not publish complete new state");
      completed = true;
      break;
    }
  }
  allocation_faults += faults;
  Check(completed && faults >= 2, "aggregate initializer allocation sweep did not reach completion");
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
  CallBinding();
  BinaryDiagnostics();
  BinaryAggregate();
  std::cout << checks << " checks, " << allocation_faults << " allocation faults, " << failures << " failures\n";
  return failures ? 1 : 0;
}
