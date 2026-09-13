// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "registry/function_registry.hpp"
#include "registry/function_seed_registry.hpp"
#include "metadata/function_hardening.hpp"
#include "metadata/function_parser_projection.hpp"
#include "common/function_result_helpers.hpp"
#include "families/crypto_hash/crypto_hash_function_landing_zone.hpp"
#include "sblr/sblr_aggregate_window_runtime.hpp"
#include "sblr/sblr_function_diagnostic.hpp"
#include "sblr/sblr_block_runtime.hpp"
#include "internal_api/api_types.hpp"
#include "blake3_digest.hpp"
#include "scrypt_kdf.hpp"
#include "../../src/core/common/crypto_random.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <thread>
#include <type_traits>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace f = scratchbird::engine::functions;
namespace s = scratchbird::engine::sblr;
namespace {
std::atomic<long> fail_after{-1};
unsigned checks = 0, failures = 0, allocation_faults = 0;
thread_local bool rng_armed=false;
thread_local int rng_result=1, rng_prefix=16, rng_interceptions=0;
thread_local bool rng_watch=false, rng_cleared=false;
thread_local void* rng_scratch=nullptr;
thread_local std::size_t rng_cleansed_bytes=0;
thread_local int rng_requested=0;
thread_local bool digest_armed=false;
thread_local int digest_result=1, digest_interceptions=0;
thread_local unsigned digest_length=32;
thread_local bool hmac_armed=false, hmac_watch=false;
thread_local int hmac_return=1, hmac_interceptions=0;
thread_local unsigned hmac_length=32, hmac_cleanses=0;
thread_local void* hmac_scratch=nullptr;
thread_local bool hmac_cleared=false;
thread_local bool scrypt_armed=false,scrypt_watch=false,scrypt_cleared=false;
thread_local int scrypt_return=1,scrypt_interceptions=0;
thread_local std::size_t scrypt_prefix=0,scrypt_scratch_size=0,scrypt_cleanses=0;
thread_local void* scrypt_scratch=nullptr;
thread_local std::array<std::uint64_t,4> scrypt_parameters{};
thread_local std::size_t scrypt_expected_workspace=2432;
thread_local std::array<unsigned char,64> secret_expected{};
thread_local void* scrypt_workspace_allocation=nullptr;
thread_local unsigned secret_allocation_count=0;
thread_local bool prepared_while_workspace_live=false;
struct SecretAllocation {void* address=nullptr;std::size_t size=0;};
thread_local std::array<SecretAllocation,128> secret_allocations{};
thread_local bool secret_deallocation_watch=false,secret_tracking_overflow=false;
thread_local unsigned uncleared_secret_frees=0;
void ObserveSecretAllocation(void* address,std::size_t size) noexcept {
  if(scrypt_watch&&scrypt_expected_workspace&&size==scrypt_expected_workspace){scrypt_scratch=address;scrypt_scratch_size=size;scrypt_workspace_allocation=address;}
  if(!secret_deallocation_watch||size!=64)return;
  if(++secret_allocation_count==2&&scrypt_workspace_allocation)prepared_while_workspace_live=true;
  for(auto& slot:secret_allocations)if(!slot.address){slot={address,size};return;}
  secret_tracking_overflow=true;
}
void ReleaseObservedAllocation(void* address) noexcept {
  if(address==scrypt_workspace_allocation)scrypt_workspace_allocation=nullptr;
  if(!secret_deallocation_watch){std::free(address);return;}
  for(auto& slot:secret_allocations)if(slot.address==address&&address) {
    if(secret_deallocation_watch) {
      bool secret=true;
      for(std::size_t i=0;i<slot.size;++i)secret=secret&&static_cast<unsigned char*>(address)[i]==secret_expected[i];
      if(secret)++uncleared_secret_frees;
    }
    slot={};break;
  }
  std::free(address);
}
}
extern "C" int __real_EVP_PBE_scrypt(const char*,std::size_t,const unsigned char*,std::size_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,unsigned char*,std::size_t);
extern "C" int __wrap_EVP_PBE_scrypt(const char* password,std::size_t password_size,const unsigned char* salt,std::size_t salt_size,std::uint64_t n,std::uint64_t r,std::uint64_t p,std::uint64_t maxmem,unsigned char* out,std::size_t size) {
  scrypt_parameters={n,r,p,size};
  if(!scrypt_armed)return __real_EVP_PBE_scrypt(password,password_size,salt,salt_size,n,r,p,maxmem,out,size);
  scrypt_armed=false;++scrypt_interceptions;
  for(std::size_t i=0;i<size&&i<scrypt_prefix;++i)out[i]=static_cast<unsigned char>(0xb0+i);
  return scrypt_return;
}
extern "C" void __real_OPENSSL_cleanse(void*,std::size_t);
extern "C" void __wrap_OPENSSL_cleanse(void* bytes,std::size_t count) {
  __real_OPENSSL_cleanse(bytes,count);
  if(bytes==scrypt_scratch) {
    ++scrypt_cleanses;scrypt_cleared=count==scrypt_scratch_size;
    for(std::size_t i=0;i<count;++i)scrypt_cleared=scrypt_cleared&&static_cast<unsigned char*>(bytes)[i]==0;
    scrypt_scratch=nullptr;
  }
  if(bytes==rng_scratch) {
    rng_cleansed_bytes=count;rng_cleared=true;
    for(std::size_t i=0;i<count;++i)rng_cleared=rng_cleared&&static_cast<unsigned char*>(bytes)[i]==0;
    rng_scratch=nullptr;
  }
  if(bytes==hmac_scratch) {
    ++hmac_cleanses;hmac_cleared=count==EVP_MAX_MD_SIZE;
    for(std::size_t i=0;i<count;++i)hmac_cleared=hmac_cleared&&static_cast<unsigned char*>(bytes)[i]==0;
    hmac_scratch=nullptr;
  }
}
extern "C" unsigned char* __real_HMAC(const EVP_MD*,const void*,int,const unsigned char*,std::size_t,unsigned char*,unsigned int*);
extern "C" unsigned char* __wrap_HMAC(const EVP_MD* md,const void* key,int key_length,const unsigned char* data,std::size_t size,unsigned char* out,unsigned int* length) {
  if(hmac_watch)hmac_scratch=out;
  if(!hmac_armed)return __real_HMAC(md,key,key_length,data,size,out,length);
  hmac_armed=false;++hmac_interceptions;
  for(unsigned i=0;i<hmac_length&&i<EVP_MAX_MD_SIZE;++i)out[i]=static_cast<unsigned char>(0xb0+i);
  *length=hmac_length;
  return hmac_return==0?nullptr:(hmac_return==1?out:out+1);
}
extern "C" int __real_EVP_Digest(const void*,std::size_t,unsigned char*,unsigned int*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_Digest(const void* data,std::size_t count,unsigned char* out,unsigned int* length,const EVP_MD* type,ENGINE* impl) {
  if(!digest_armed)return __real_EVP_Digest(data,count,out,length,type,impl);
  digest_armed=false;++digest_interceptions;
  for(unsigned i=0;i<digest_length&&i<EVP_MAX_MD_SIZE;++i)out[i]=static_cast<unsigned char>(0xc0+i);
  *length=digest_length;
  return digest_result;
}
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* out,int count) {
  if(!rng_armed)return __real_RAND_bytes(out,count);
  if(rng_watch)rng_scratch=out;
  rng_requested=count;
  rng_armed=false;++rng_interceptions;
  for(int i=0;i<count&&i<rng_prefix;++i)out[i]=static_cast<unsigned char>(0xa0+i);
  return rng_result;
}
void* operator new(std::size_t n) {
  auto remaining = fail_after.load();
  if (remaining >= 0) {
    if (!remaining) throw std::bad_alloc();
    --fail_after;
  }
  if (auto* p = std::malloc(n ? n : 1)) {ObserveSecretAllocation(p,n);return p;}
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { ReleaseObservedAllocation(p); }
void operator delete[](void* p) noexcept { ReleaseObservedAllocation(p); }
void operator delete(void* p, std::size_t) noexcept { ReleaseObservedAllocation(p); }
void operator delete[](void* p, std::size_t) noexcept { ReleaseObservedAllocation(p); }

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
void BinaryUuidValues() {
  static_assert(sizeof(decltype(s::SblrValue::uuid_value))==16);
  static_assert(std::is_same_v<decltype(s::SblrErrorHandlerFrame::handler_uuid),s::SblrUuid>);
  for(unsigned version=1;version<=7;++version) {
    auto uuid=Base();uuid.bytes[6]=static_cast<unsigned char>((version<<4)|(uuid.bytes[6]&15));
    const auto value=s::MakeSblrUuidValue(uuid);
    Check(!value.is_null&&value.payload_kind==s::SblrValuePayloadKind::uuid_binary&&value.uuid_value==uuid,
          "UUID carrier must preserve admitted user UUID versions without rewriting identity");
    Check(value.text_value.empty()&&value.encoded_value.empty()&&value.binary_value.empty(),"UUID has one fixed binary identity payload");
    std::vector<std::uint8_t> output{99,88};
    Check(s::CopySblrUuidPayload(value,&output)&&output.size()==16&&std::equal(output.begin(),output.end(),uuid.bytes.begin()),
          "UUID response payload is exactly the original sixteen bytes");
    for(unsigned fault=0;fault<11;++fault) {
      auto bad=value;
      if(fault==0)bad.text_value="019d0000-0000-7000-8000-000000000001";
      if(fault==1)bad.encoded_value="hidden UUID override";
      if(fault==2)bad.binary_value={1};
      if(fault==3)bad.has_int64_value=true;
      if(fault==4)bad.has_uint64_value=true;
      if(fault==5)bad.has_real64_value=true;
      if(fault==6)bad.is_null=true;
      if(fault==7)bad.payload_kind=s::SblrValuePayloadKind::uuid_text;
      if(fault==8)bad.descriptor_id="text";
      if(fault==9)bad.charset_name="UTF8";
      if(fault==10)bad.collation_name="unicode";
      const auto original=output;
      Check(!s::CopySblrUuidPayload(bad,&output)&&output==original,"conflicting UUID representation must not publish any output");
    }
    Check(!s::CopySblrUuidPayload(value,nullptr),"UUID copy needs output destination");
    const auto original=output;
    fail_after=0;bool threw=false;
    try {(void)s::CopySblrUuidPayload(value,&output);}catch(const std::bad_alloc&){threw=true;}
    fail_after=-1;
    if(threw)++allocation_faults;
    Check(threw&&output==original,"UUID output allocation failure preserves destination");
  }
  const auto nil=s::MakeSblrUuidValue({});
  Check(!nil.is_null&&nil.uuid_value.is_nil(),"nil UUID bits are distinct from SQL null; descriptor policy owns admission");
  s::SblrExecutionContext context;
  s::SblrErrorHandlerFrame handler;handler.handler_uuid=Base();handler.match_code="P0001";
  auto failure=s::RaiseSblrError("test",context,"TEST.ERROR","test source","P0001");
  auto selected=s::SelectSblrErrorHandler("test",{handler},failure,context);
  Check(selected.ok()&&selected.scalar_values.size()==1&&selected.scalar_values[0].uuid_value==handler.handler_uuid&&
        selected.scalar_values[0].payload_kind==s::SblrValuePayloadKind::uuid_binary,"handler selection carries binary frame UUID");
  for(auto& field:failure.diagnostics[0].fields)if(field.key=="sqlstate")field.value=Base();
  Check(!s::SblrErrorHandlerMatches(handler,failure),"binary diagnostic field cannot masquerade as SQLSTATE text");
  s::SblrFrameStack stack;
  Check(s::EnterSblrErrorHandler(&stack,handler).ok()&&stack.frames.size()==1&&stack.frames[0].frame_uuid==handler.handler_uuid,
        "handler entry retains exact binary frame");
  Check(s::UnwindSblrFramesForErrorHandler("test",&stack,handler.handler_uuid,context).ok()&&stack.frames.size()==1,
        "frame selector compares binary identity");
  Check(s::LeaveSblrErrorHandler(&stack).ok()&&stack.frames.empty(),"frame exit retains lifecycle accounting");
}
void CryptoUuidGeneration() {
  const auto package=f::BuildStandardFunctionSeedPackage();
  f::FunctionCallRequest request;
  request.context.function_uuid={{0x01,0x9d,0xff,0xbb,0xf0,0x00,0x76,0x15,0xba,0x9c,0x4d,0xa4,0x76,0x32,0x27,0x45}};
  Check(package.registry.BindCallContext(request.context)!=nullptr&&request.context.function_id=="sb.crypto.gen_random_uuid",
        "crypto UUID invocation binds through the actual fixed binary seed");
  request.context.sblr_context.deterministic_uuid_text="019d0000-0000-7000-8000-000000000001";
  rng_armed=true;rng_prefix=16;rng_result=1;rng_interceptions=0;
  const auto generated=f::DispatchCryptoHashFunction(request);
  Check(!rng_armed&&rng_interceptions==1,"UUID generator cannot bypass Core entropy using a text override");
  rng_armed=false;
  Check(generated.result.ok()&&generated.result.scalar_values.size()==1&&generated.result.rows.empty(),"Core RNG supplies one UUID result");
  if(!generated.result.scalar_values.empty()) {
    const auto& value=generated.result.scalar_values.front();
    s::SblrUuid expected;
    for(unsigned i=0;i<16;++i)expected.bytes[i]=static_cast<unsigned char>(0xa0+i);
    expected.bytes[6]=0x46;expected.bytes[8]=0xa8;
    Check(value.payload_kind==s::SblrValuePayloadKind::uuid_binary&&value.uuid_value==expected&&
          value.text_value.empty()&&value.encoded_value.empty()&&value.binary_value.empty(),
          "UUIDv4 retains provider bits except RFC version and variant, without text mirrors");
  }
  for(int result:{0,-1})for(int prefix=0;prefix<=16;++prefix) {
    rng_armed=true;rng_prefix=prefix;rng_result=result;rng_interceptions=0;
    const auto failed=f::DispatchCryptoHashFunction(request);
    Check(!rng_armed&&rng_interceptions==1&&!failed.result.ok()&&failed.result.scalar_values.empty()&&failed.result.rows.empty(),
          "every partial RNG failure emits no UUID or partial data");
    rng_armed=false;
    Check(failed.result.diagnostics.size()==1&&failed.result.diagnostics[0].diagnostic_id=="CRYPTO.RNG.UNAVAILABLE",
          "RNG failure has its exact canonical diagnostic");
  }
  request.arguments.push_back({"forbidden",f::MakeInt64Value("int64",1)});
  rng_armed=true;rng_result=1;rng_prefix=16;rng_interceptions=0;
  const auto arity=f::DispatchCryptoHashFunction(request);
  // Error occurrence issuance may itself use the RNG. Count only the semantic
  // result: no UUID is returned and the failure is not replaced by RNG failure.
  rng_armed=false;
  Check(!arity.result.ok()&&arity.result.scalar_values.empty(),"wrong arity cannot generate UUID success");
  request.arguments.clear();
  auto previous=s::SblrUuid{};
  for(unsigned i=0;i<64;++i) {
    const auto real=f::DispatchCryptoHashFunction(request);
    Check(real.result.ok()&&real.result.scalar_values.size()==1,"actual unmodified Core RNG executes");
    if(real.result.scalar_values.empty())continue;
    const auto& uuid=real.result.scalar_values[0].uuid_value;
    Check((uuid.bytes[6]>>4)==4&&(uuid.bytes[8]&0xc0)==0x80&&uuid!=previous,"actual Core UUIDv4 shape and successive identity");
    previous=uuid;
  }
  for(const char* name:{"sb.crypto.blake2b","sb.crypto.sha3_256","sb.crypto.sha3_512","sb.crypto.hmac","sb.crypto.scrypt","sb.crypto.xxhash64","sb.crypto.pgcrypto"}) {
    const auto* entry=package.registry.Lookup(name);
    Check(entry!=nullptr,"crypto arity fixture has an actual seed binding");
    if(!entry)continue;
    request.context.function_uuid=entry->function_uuid;
    Check(package.registry.BindCallContext(request.context)!=nullptr,"crypto arity fixture binds through binary identity");
    const auto invalid=f::DispatchCryptoHashFunction(request);
    Check(!invalid.result.ok()&&invalid.result.scalar_values.empty()&&invalid.result.rows.empty(),"crypto zero-argument probe cannot return a fabricated success marker");
    if(request.context.function_id=="sb.crypto.pgcrypto")Check(invalid.result.diagnostics.size()==1&&invalid.result.diagnostics[0].diagnostic_id=="CRYPTO.PACKAGE.NOT_CALLABLE",
        "package marker uses its specified noncallable diagnostic");
  }
}
void CryptoFixedDigests() {
  const auto package=f::BuildStandardFunctionSeedPackage();
  struct Known {const char* name;const char* hex;};
  const Known cases[]{
    {"sb.crypto.blake3","e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
    {"sb.crypto.blake2b","ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"},
    {"sb.crypto.sha3_256","3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},
    {"sb.crypto.sha3_512","b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"}
  };
  for(const auto& known:cases) {
    f::FunctionCallRequest request;
    const auto* entry=package.registry.Lookup(known.name);
    Check(entry!=nullptr,"fixed digest has a registered identity");if(!entry)continue;
    request.context.function_uuid=entry->function_uuid;
    Check(package.registry.BindCallContext(request.context)!=nullptr,"fixed digest binds exact binary identity");
    const bool local_blake3=std::string_view(known.name)=="sb.crypto.blake3";
    const auto input=f::MakeBinaryValue("binary",local_blake3 ? std::vector<std::uint8_t>{0,1,2} : std::vector<std::uint8_t>{'a','b','c'});
    request.arguments={{"value",input}};
    const auto actual=f::DispatchCryptoHashFunction(request);
    std::vector<std::uint8_t> expected;
    const std::string_view hex=known.hex;
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<hex.size();i+=2)expected.push_back(static_cast<std::uint8_t>(nibble(hex[i])*16+nibble(hex[i+1])));
    Check(actual.result.ok()&&actual.result.scalar_values.size()==1,"fixed digest actual provider succeeds");
    if(!actual.result.scalar_values.empty()) {
      const auto& value=actual.result.scalar_values[0];
      Check(value.descriptor_id=="binary"&&value.payload_kind==s::SblrValuePayloadKind::binary&&value.binary_value==expected&&
            value.text_value.empty()&&value.encoded_value.empty()&&!value.is_null,"fixed known answer is binary digest, never hex text");
    }
    request.arguments[0].value=f::MakeBinaryValue("binary",{});
    const auto empty=f::DispatchCryptoHashFunction(request);
    Check(empty.result.ok()&&empty.result.scalar_values.size()==1&&!empty.result.scalar_values[0].is_null&&
          empty.result.scalar_values[0].binary_value.size()==expected.size()&&empty.result.scalar_values[0].binary_value!=expected,
          "empty byte sequence is hashed, not treated as SQL NULL or constant abc result");
    request.arguments[0].value=f::MakeNullValue("binary");
    digest_armed=true;digest_interceptions=0;
    const auto null=f::DispatchCryptoHashFunction(request);digest_armed=false;
    Check(null.result.ok()&&null.result.scalar_values.size()==1&&null.result.scalar_values[0].is_null&&
          null.result.scalar_values[0].descriptor_id=="binary"&&digest_interceptions==0,"strict typed NULL avoids provider evaluation");
    for(unsigned fault=0;fault<13;++fault) {
      auto bad=input;
      if(fault==0)bad.descriptor_id="character";
      if(fault==1)bad.payload_kind=s::SblrValuePayloadKind::text;
      if(fault==2)bad.text_value="hidden";
      if(fault==3)bad.encoded_value="hidden";
      if(fault==4)bad.charset_name="UTF8";
      if(fault==5)bad.collation_name="unicode";
      if(fault==6)bad.has_int64_value=true;
      if(fault==7)bad.has_uint64_value=true;
      if(fault==8)bad.has_real64_value=true;
      if(fault==9)bad.uuid_value=Base();
      if(fault==10)bad.is_null=true;
      if(fault==11){bad=f::MakeNullValue("binary");bad.binary_value={1};}
      if(fault==12)bad.binary_value.resize(1048577);
      request.arguments[0].value=std::move(bad);
      digest_armed=true;digest_interceptions=0;
      const auto invalid=f::DispatchCryptoHashFunction(request);digest_armed=false;
      Check(!invalid.result.ok()&&invalid.result.scalar_values.empty()&&digest_interceptions==0&&
            invalid.result.diagnostics[0].diagnostic_id=="CRYPTO.HASH.INVALID_INPUT","malformed digest input never invokes provider or produces output");
    }
    request.arguments[0].value=input;
    for(unsigned length:{0u,1u,31u,32u,33u,63u,64u,65u,~0u}) {
      if(local_blake3)break;  // The local algorithm does not call EVP_Digest.
      if(length==expected.size())continue;
      digest_length=length;digest_result=1;digest_armed=true;digest_interceptions=0;
      const auto invalid=f::DispatchCryptoHashFunction(request);digest_armed=false;
      Check(!invalid.result.ok()&&invalid.result.scalar_values.empty()&&digest_interceptions==1&&
            invalid.result.diagnostics[0].diagnostic_id=="CRYPTO.PROFILE.UNAVAILABLE","wrong provider output length cannot publish a partial or oversized digest");
    }
    for(int result:{0,-1}) {
      if(local_blake3)break;
      digest_length=13;digest_result=result;digest_armed=true;digest_interceptions=0;
      const auto invalid=f::DispatchCryptoHashFunction(request);digest_armed=false;
      Check(!invalid.result.ok()&&invalid.result.scalar_values.empty()&&digest_interceptions==1&&
            invalid.result.diagnostics[0].diagnostic_id=="CRYPTO.PROFILE.UNAVAILABLE","partial provider failure never publishes digest bytes");
    }
    bool completed=false;unsigned faults=0;
    for(long budget=0;budget<100;++budget) {
      fail_after=budget;
      try {
        const auto result=f::DispatchCryptoHashFunction(request);fail_after=-1;
        Check(result.result.ok()&&result.result.scalar_values.size()==1&&result.result.scalar_values[0].binary_value==expected,
              "digest allocation sweep reaches complete known-answer result");completed=true;break;
      }catch(const std::bad_alloc&){fail_after=-1;++faults;}
      Check(request.arguments[0].value.binary_value==input.binary_value,"digest allocation failures preserve input payload");
    }
    allocation_faults+=faults;
    Check(completed&&faults>0,"digest allocation sweep reaches successful publication");
  }
}

void Blake3KnownAnswers() {
  // Fixed reference facts, not output generated by the implementation under test.
  // https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json
  // Message byte i is i modulo 251; only unkeyed 256-bit outputs apply here.
  struct Known {std::size_t size; const char* hex;};
  const Known cases[]{
    {0,"af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
    {1,"2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
    {63,"e9bc37a594daad83be9470df7f7b3798297c3d834ce80ba85d6e207627b7db7b"},
    {64,"4eed7141ea4a5cd4b788606bd23f46e212af9cacebacdc7d1f4c6dc7f2511b98"},
    {65,"de1e5fa0be70df6d2be8fffd0e99ceaa8eb6e8c93a63f2d8d1c30ecb6b263dee"},
    {127,"d81293fda863f008c09e92fc382a81f5a0b4a1251cba1634016a0f86a6bd640d"},
    {128,"f17e570564b26578c33bb7f44643f539624b05df1a76c81f30acd548c44b45ef"},
    {129,"683aaae9f3c5ba37eaaf072aed0f9e30bac0865137bae68b1fde4ca2aebdcb12"},
    {1023,"10108970eeda3eb932baac1428c7a2163b0e924c9a9e25b35bba72b28f70bd11"},
    {1024,"42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
    {1025,"d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444"},
    {2048,"e776b6028c7cd22a4d0ba182a8bf62205d2ef576467e838ed6f2529b85fba24a"},
    {2049,"5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030"},
    {3072,"b98cb0ff3623be03326b373de6b9095218513e64f1ee2edd2525c7ad1e5cffd2"},
    {3073,"7124b49501012f81cc7f11ca069ec9226cecb8a2c850cfe644e327d22d3e1cd3"},
    {4096,"015094013f57a5277b59d8475c0501042c0b642e531b0a1c8f58d2163229e969"},
    {4097,"9b4052b38f1c5fc8b1f9ff7ac7b27cd242487b3d890d15c96a1c25b8aa0fb995"},
    {5120,"9cadc15fed8b5d854562b26a9536d9707cadeda9b143978f319ab34230535833"},
    {5121,"628bd2cb2004694adaab7bbd778a25df25c47b9d4155a55f8fbd79f2fe154cff"},
    {6144,"3e2e5b74e048f3add6d21faab3f83aa44d3b2278afb83b80b3c35164ebeca205"},
    {6145,"f1323a8631446cc50536a9f705ee5cb619424d46887f3c376c695b70e0f0507f"},
    {7168,"61da957ec2499a95d6b8023e2b0e604ec7f6b50e80a9678b89d2628e99ada77a"},
    {7169,"a003fc7a51754a9b3c7fae0367ab3d782dccf28855a03d435f8cfe74605e7817"},
    {8192,"aae792484c8efe4f19e2ca7d371d8c467ffb10748d8a5a1ae579948f718a2a63"},
    {8193,"bab6c09cb8ce8cf459261398d2e7aef35700bf488116ceb94a36d0f5f1b7bc3b"},
    {16384,"f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4"},
    {31744,"62b6960e1a44bcc1eb1a611a8d6235b6b4b78f32e7abc4fb4c6cdcce94895c47"},
    {102400,"bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"}
  };
  const auto package=f::BuildStandardFunctionSeedPackage();
  const auto* entry=package.registry.Lookup("sb.crypto.blake3");
  Check(entry!=nullptr,"BLAKE3 reference test has actual seed binding");if(!entry)return;
  f::FunctionCallRequest request;request.context.function_uuid=entry->function_uuid;
  Check(package.registry.BindCallContext(request.context)!=nullptr,"BLAKE3 binds through binary function identity");
  for(const auto& known:cases) {
    std::vector<std::uint8_t> input(known.size);
    for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<std::uint8_t>(i%251);
    const auto saved=input;
    scratchbird::core::hash::Blake3Digest expected{};
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>(nibble(known.hex[2*i])*16+nibble(known.hex[2*i+1]));
    fail_after=0;
    const auto actual=scratchbird::core::hash::ComputeBlake3Digest(input);
    const bool no_allocation=fail_after.load()==0;fail_after=-1;
    Check(actual==expected&&no_allocation,"allocation-free BLAKE3 Core computation matches independent vector");
    Check(input==saved,"BLAKE3 leaves caller-owned bytes unchanged");
    request.arguments={{"value",f::MakeBinaryValue("binary",input)}};
    digest_armed=true;digest_interceptions=0;
    const auto result=f::DispatchCryptoHashFunction(request);digest_armed=false;
    Check(result.result.ok()&&result.result.scalar_values.size()==1&&digest_interceptions==0,
          "BLAKE3 executes locally without an external digest provider");
    if(!result.result.scalar_values.empty()) {
      const auto& value=result.result.scalar_values[0];
      Check(value.binary_value.size()==32&&std::equal(value.binary_value.begin(),value.binary_value.end(),expected.begin())&&
            value.descriptor_id=="binary"&&value.payload_kind==s::SblrValuePayloadKind::binary&&
            !value.is_null&&value.text_value.empty()&&value.encoded_value.empty(),"BLAKE3 family publishes complete binary known answer");
    }
    if(!input.empty()) {
      input.back()^=0x80;
      Check(scratchbird::core::hash::ComputeBlake3Digest(input)!=expected,"BLAKE3 includes the final byte of every extent");
      input=saved;
    }
    input.push_back(0);
    Check(scratchbird::core::hash::ComputeBlake3Digest(input)!=expected,"BLAKE3 length distinguishes trailing zero from padding");
    std::atomic<unsigned> mismatches{0};
    std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t)threads.emplace_back([&]{
      for(unsigned repetition=0;repetition<4;++repetition)
        if(scratchbird::core::hash::ComputeBlake3Digest(saved)!=expected)++mismatches;
    });
    for(auto& thread:threads)thread.join();
    Check(mismatches==0,"concurrent BLAKE3 calls have no shared mutable digest state");
  }
}
void CryptoHmac() {
  struct Profile {const char* name;const EVP_MD*(*digest)();std::size_t block;const char* known;};
  // SHA2 fixed values: RFC4231 case1. SHA3/BLAKE2b: independent inner/outer
  // digest composition, also checked below without invoking an HMAC provider.
  const Profile profiles[]{
    {"sha256",EVP_sha256,64,"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
    {"sha512",EVP_sha512,128,"87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"},
    {"sha3_256",EVP_sha3_256,136,"ba85192310dffa96e2a3a40e69774351140bb7185e1202cdcc917589f95e16bb"},
    {"sha3_512",EVP_sha3_512,72,"eb3fbd4b2eaab8f5c504bd3a41465aacec15770a7cabac531e482f860b5ec7ba47ccb2c6f2afce8f88d22b6dc61380f23a668fd3888bb80537c0a0b86407689e"},
    {"blake2b",EVP_blake2b512,128,"358a6a184924894fc34bee5680eedf57d84a37bb38832f288e3b27dc63a98cc8c91e76da476b508bc6b2d408a248857452906e4a20b48c6b4b55d2df0fe1dd24"}
  };
  const auto package=f::BuildStandardFunctionSeedPackage();
  const auto refusal=[](const f::FunctionCallResult& result,const char* code) {
    return !result.result.ok()&&result.result.scalar_values.empty()&&!result.result.diagnostics.empty()&&
           result.result.diagnostics[0].diagnostic_id==code;
  };
  const auto output=[](const f::FunctionCallResult& result,const std::vector<std::uint8_t>& bytes) {
    if(!result.result.ok()||result.result.scalar_values.size()!=1)return false;
    const auto& value=result.result.scalar_values[0];
    return !value.is_null&&value.descriptor_id=="binary"&&value.payload_kind==s::SblrValuePayloadKind::binary&&
           value.binary_value==bytes&&value.text_value.empty()&&value.encoded_value.empty();
  };
  for(const char* function:{"sb.crypto.hmac","sb.crypto.hmac_value_key_algo"}) {
    const auto* entry=package.registry.Lookup(function);Check(entry!=nullptr,"HMAC identity exists");if(!entry)continue;
    f::FunctionCallRequest request;request.context.function_uuid=entry->function_uuid;
    Check(package.registry.BindCallContext(request.context)!=nullptr,"HMAC alias and base bind their actual binary identities");
    for(const auto& profile:profiles) {
      const std::vector<std::uint8_t> data{'H','i',' ','T','h','e','r','e'}, key(20,0x0b);
      std::vector<std::uint8_t> expected;
      const std::string_view hex=profile.known;
      auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
      for(std::size_t i=0;i<hex.size();i+=2)expected.push_back(static_cast<std::uint8_t>(nibble(hex[i])*16+nibble(hex[i+1])));
      request.arguments={{"value",f::MakeBinaryValue("binary",data)},{"key",f::MakeBinaryValue("binary",key)},
                         {"algorithm",f::MakeTextValue("character",profile.name)}};
      hmac_watch=true;hmac_cleanses=0;hmac_cleared=false;
      Check(output(f::DispatchCryptoHashFunction(request),expected),"HMAC actual provider publishes fixed binary known answer");
      Check(hmac_cleanses==1&&hmac_cleared,"successful HMAC clears provider scratch");hmac_watch=false;
      const auto original=request.arguments;
      for(unsigned null_index=0;null_index<3;++null_index) {
        request.arguments=original;request.arguments[null_index].value=f::MakeNullValue(null_index==2?"character":"binary");
        hmac_armed=true;hmac_interceptions=0;
        const auto result=f::DispatchCryptoHashFunction(request);hmac_armed=false;
        Check(result.result.ok()&&result.result.scalar_values.size()==1&&result.result.scalar_values[0].is_null&&
              result.result.scalar_values[0].descriptor_id=="binary"&&hmac_interceptions==0,"HMAC strict typed NULL does not invoke provider");
      }
      request.arguments=original;
      for(unsigned length:{0u,1u,31u,32u,33u,63u,64u,65u,~0u}) {
        if(length==expected.size())continue;
        hmac_watch=true;hmac_cleanses=0;hmac_cleared=false;hmac_armed=true;hmac_length=length;hmac_return=1;
        Check(refusal(f::DispatchCryptoHashFunction(request),"CRYPTO.PROFILE.UNAVAILABLE"),"HMAC never trusts short, long or oversized provider lengths");
        Check(hmac_cleanses==1&&hmac_cleared,"HMAC wrong-length refusal clears scratch");hmac_armed=false;hmac_watch=false;
      }
      for(int result:{0,2}) {
        hmac_watch=true;hmac_cleanses=0;hmac_cleared=false;hmac_armed=true;hmac_length=13;hmac_return=result;
        Check(refusal(f::DispatchCryptoHashFunction(request),"CRYPTO.PROFILE.UNAVAILABLE"),"HMAC partial failure or foreign result pointer never publishes bytes");
        Check(hmac_cleanses==1&&hmac_cleared,"HMAC failed provider clears scratch");hmac_armed=false;hmac_watch=false;
      }
      for(unsigned slot=0;slot<3;++slot)for(unsigned fault=0;fault<10;++fault) {
        request.arguments=original;auto& bad=request.arguments[slot].value;
        if(fault==0)bad.descriptor_id="int64";
        if(fault==1)bad.payload_kind=s::SblrValuePayloadKind::uuid_binary;
        if(fault==2)bad.has_int64_value=true;
        if(fault==3)bad.has_uint64_value=true;
        if(fault==4)bad.has_real64_value=true;
        if(fault==5)bad.uuid_value=Base();
        if(fault==6)bad.is_null=true;
        if(fault==7)bad.encoded_value="secret-conflicting-mirror";
        if(fault==8){if(slot==2)bad.binary_value={1};else bad.charset_name="UTF8";}
        if(fault==9){if(slot==2)bad.descriptor_id="binary";else bad=f::MakeTextValue("character","secret-key");}
        hmac_armed=true;hmac_interceptions=0;
        const auto result=f::DispatchCryptoHashFunction(request);hmac_armed=false;
        Check(refusal(result,"CRYPTO.HMAC.INVALID_INPUT")&&hmac_interceptions==0,"invalid HMAC representations never reach provider");
        if(!result.result.diagnostics.empty())Check(result.result.diagnostics[0].detail.find("secret")==std::string::npos,"HMAC diagnostics do not disclose input or key");
      }
      request.arguments=original;
      for(const auto& token:std::vector<std::string>{"", "sha3_256()", "sha256 ", " sha256", "sha-256", "sha1", "md5", "blake3",std::string("sha256\0",7),"SHA\xc4\xb0"}) {
        request.arguments[2].value=f::MakeTextValue("character",token);hmac_armed=true;hmac_interceptions=0;
        Check(refusal(f::DispatchCryptoHashFunction(request),"CRYPTO.HMAC.UNSUPPORTED_ALGORITHM")&&hmac_interceptions==0,"HMAC algorithm tokens use exact ASCII profile vocabulary");hmac_armed=false;
      }
      for(const auto& token:std::vector<std::string>{profile.name,std::string(profile.name)=="sha3_256"?"ShA3-256":std::string(profile.name)=="sha3_512"?"SHA3-512":std::string(profile.name)=="blake2b"?"BLAKE2B512":std::string(profile.name)=="sha256"?"SHA256":"SHA512"}) {
        request.arguments[2].value=f::MakeTextValue("character",token);
        Check(output(f::DispatchCryptoHashFunction(request),expected),"HMAC profile aliases and ASCII case preserve exact digest");
      }
      // Independent construction over the raw digest API, never HMAC(). Covers
      // empty keys/data and key hashing on both sides of each block boundary.
      const auto raw_hash=[&](const std::vector<std::uint8_t>& bytes) {
        std::vector<std::uint8_t> result(EVP_MAX_MD_SIZE);unsigned count=0;
        Check(EVP_Digest(bytes.data(),bytes.size(),result.data(),&count,profile.digest(),nullptr)==1,"independent HMAC construction digest succeeds");
        result.resize(count);return result;
      };
      for(std::size_t length:{std::size_t{0},std::size_t{1},profile.block-1,profile.block,profile.block+1,std::size_t{8193}}) {
        std::vector<std::uint8_t> input(length), material(length);
        for(std::size_t i=0;i<length;++i){input[i]=static_cast<std::uint8_t>(i*17);material[i]=static_cast<std::uint8_t>(i*31);}
        auto padded=material.size()>profile.block?raw_hash(material):material;padded.resize(profile.block);
        std::vector<std::uint8_t> inner(profile.block),outer(profile.block);
        for(std::size_t i=0;i<profile.block;++i){inner[i]=padded[i]^0x36;outer[i]=padded[i]^0x5c;}
        inner.insert(inner.end(),input.begin(),input.end());const auto inner_digest=raw_hash(inner);
        outer.insert(outer.end(),inner_digest.begin(),inner_digest.end());const auto wanted=raw_hash(outer);
        for(bool text:{false,true}) {
          auto value=text?f::MakeTextValue("character",std::string(input.begin(),input.end())):f::MakeBinaryValue("binary",input);
          auto secret=text?f::MakeTextValue("character",std::string(material.begin(),material.end())):f::MakeBinaryValue("binary",material);
          if(text){value.charset_name="ISO8859_1";secret.charset_name="ISO8859_1";value.collation_name="does_not_transform_hmac_bytes";}
          request.arguments={{"value",value},{"key",secret},{"algorithm",f::MakeTextValue("character",profile.name)}};
          Check(output(f::DispatchCryptoHashFunction(request),wanted),"binary and descriptor-encoded text HMAC use every byte including NUL and high bytes");
        }
      }
      request.arguments=original;
      bool completed=false;unsigned faults=0;
      for(long budget=0;budget<100;++budget) {
        hmac_watch=true;hmac_cleanses=0;hmac_cleared=false;fail_after=budget;
        try {const auto result=f::DispatchCryptoHashFunction(request);fail_after=-1;
          Check(output(result,expected),"HMAC allocation sweep reaches known answer");completed=true;
        }catch(const std::bad_alloc&){fail_after=-1;++faults;}
        hmac_watch=false;
        Check(hmac_scratch==nullptr&&(hmac_cleanses==0||hmac_cleared),"HMAC allocation failure cannot leave uncleared provider scratch");
        Check(request.arguments[1].value.binary_value==key,"HMAC allocation failure preserves caller key");
        if(completed)break;
      }
      allocation_faults+=faults;Check(completed&&faults>0,"HMAC allocation faults exercised through publication");
      for(std::size_t arity:{std::size_t{0},std::size_t{1},std::size_t{2},std::size_t{4}}) {
        request.arguments=original;request.arguments.resize(arity);
        Check(refusal(f::DispatchCryptoHashFunction(request),"CRYPTO.HMAC.INVALID_INPUT"),"HMAC enforces exactly three arguments");
      }
    }
  }
}

void CryptoRandomBytes() {
  const auto package=f::BuildStandardFunctionSeedPackage();
  const auto refuse=[](const f::FunctionCallResult& result,const char* code) {
    return !result.result.ok()&&result.result.scalar_values.empty()&&!result.result.diagnostics.empty()&&result.result.diagnostics[0].diagnostic_id==code;
  };
  const auto arm=[](int result,int prefix) {
    rng_armed=true;rng_result=result;rng_prefix=prefix;rng_interceptions=0;rng_requested=0;
    rng_watch=true;rng_scratch=nullptr;rng_cleared=false;rng_cleansed_bytes=0;
  };
  const auto disarm=[] {rng_armed=false;rng_watch=false;};
  // Refusal diagnostics can issue a fresh UUID and legitimately request16
  // entropy bytes on a new clock tick. That is not random-byte result work.
  const auto diagnostic_entropy_only=[] {return rng_interceptions==0||(rng_interceptions==1&&rng_requested==16);};
  for(const char* name:{"sb.crypto.gen_random_bytes","sb.crypto.gen_random_bytes_n"}) {
    const auto* entry=package.registry.Lookup(name);Check(entry!=nullptr,"random-byte function has actual registry identity");if(!entry)continue;
    f::FunctionCallRequest request;request.context.function_uuid=entry->function_uuid;
    Check(package.registry.BindCallContext(request.context)!=nullptr,"random-byte alias and base bind actual binary identities");
    request.context.sblr_context.deterministic_random_bytes_hex=std::string(2048,'f');
    for(unsigned length=1;length<=1024;++length) {
      request.arguments={{"count",f::MakeUint64Value("uint32",length)}};
      if(length%2==0){request.arguments[0].value.text_value.clear();request.arguments[0].value.encoded_value.clear();}
      arm(1,static_cast<int>(length));
      const auto result=f::DispatchCryptoHashFunction(request);disarm();
      Check(result.result.ok()&&result.result.scalar_values.size()==1&&rng_interceptions==1&&rng_requested==static_cast<int>(length),"every admitted random-byte length requests actual entropy despite context override");
      if(!result.result.scalar_values.empty()) {
        const auto& value=result.result.scalar_values[0];bool exact=value.binary_value.size()==length;
        for(std::size_t i=0;i<value.binary_value.size();++i)exact=exact&&value.binary_value[i]==static_cast<unsigned char>(0xa0+i);
        Check(exact&&!value.is_null&&value.descriptor_id=="binary"&&value.payload_kind==s::SblrValuePayloadKind::binary&&value.text_value.empty()&&value.encoded_value.empty(),"random-byte publication is exact binary entropy, never the request hex override");
      }
      Check(rng_scratch==nullptr&&rng_cleared&&rng_cleansed_bytes==1024,"random-byte success cleanses all owned entropy scratch");
    }
    for(unsigned length:{1u,16u,255u,1024u})for(int result:{0,-1})for(unsigned prefix:{0u,1u,length/2,length}) {
      request.arguments={{"count",f::MakeUint64Value("uint32",length)}};arm(result,static_cast<int>(prefix));
      const auto failed=f::DispatchCryptoHashFunction(request);disarm();
      Check(refuse(failed,"CRYPTO.RNG.UNAVAILABLE")&&rng_interceptions==1,"partial failed entropy produces no binary prefix or fallback");
      Check(rng_scratch==nullptr&&rng_cleared&&rng_cleansed_bytes==length,"Core shared entropy helper clears failed provider output");
    }
    for(auto length:{std::uint64_t{0},std::uint64_t{1025},std::uint64_t{0xffffffff},std::uint64_t{0x100000000},~std::uint64_t{0}}) {
      request.arguments={{"count",f::MakeUint64Value("uint32",length)}};arm(1,1024);
      Check(refuse(f::DispatchCryptoHashFunction(request),"CRYPTO.RNG.INVALID_LENGTH")&&diagnostic_entropy_only(),"random-byte invalid lengths never generate result entropy or narrow uint64");disarm();
    }
    for(unsigned fault=0;fault<15;++fault) {
      auto count=f::MakeUint64Value("uint32",32);
      if(fault==0)count.descriptor_id="uint64";
      if(fault==1)count=f::MakeInt64Value("int32",16);
      if(fault==2)count=f::MakeTextValue("character","16");
      if(fault==3)count.payload_kind=s::SblrValuePayloadKind::text;
      if(fault==4)count.has_uint64_value=false;
      if(fault==5)count.has_int64_value=true;
      if(fault==6)count.has_real64_value=true;
      if(fault==7)count.uuid_value=Base();
      if(fault==8)count.binary_value={16};
      if(fault==9)count.charset_name="UTF8";
      if(fault==10)count.collation_name="binary";
      if(fault==11)count.text_value="15";
      if(fault==12)count.encoded_value="016";
      if(fault==13)count.is_null=true;
      if(fault==14){count=f::MakeNullValue("uint32");count.text_value="secret-count";}
      request.arguments={{"count",count}};arm(1,16);
      Check(refuse(f::DispatchCryptoHashFunction(request),"CRYPTO.RNG.INVALID_LENGTH")&&diagnostic_entropy_only(),"malformed random-byte count carrier is never parsed/coerced");disarm();
    }
    for(unsigned arity:{0u,2u}) {
      request.arguments.resize(arity);arm(1,16);
      Check(refuse(f::DispatchCryptoHashFunction(request),"CRYPTO.RNG.INVALID_LENGTH")&&diagnostic_entropy_only(),"random-byte count is mandatory for both aliases");disarm();
    }
    request.arguments={{"count",f::MakeNullValue("uint32")}};arm(1,16);
    const auto null=f::DispatchCryptoHashFunction(request);disarm();
    Check(null.result.ok()&&null.result.scalar_values.size()==1&&null.result.scalar_values[0].is_null&&null.result.scalar_values[0].descriptor_id=="binary"&&rng_interceptions==0,"random-byte strict uint32 NULL is binary NULL without entropy");
    request.arguments={{"count",f::MakeUint64Value("uint32",32)}};
    std::vector<std::uint8_t> previous;
    for(unsigned i=0;i<16;++i) {
      const auto result=f::DispatchCryptoHashFunction(request);
      Check(result.result.ok()&&result.result.scalar_values.size()==1&&result.result.scalar_values[0].binary_value.size()==32,"real unmodified Core RNG returns requested bytes");
      if(!result.result.scalar_values.empty()) {
        const auto& bytes=result.result.scalar_values[0].binary_value;
        Check(bytes!=previous&&bytes!=std::vector<std::uint8_t>(32,0xff),"real random-byte generation does not replay prior bytes or context fixture");previous=bytes;
      }
    }
    for(bool failure:{false,true}) {
      unsigned faults=0;bool completed=false;
      for(long budget=0;budget<100;++budget) {
        arm(failure?0:1,failure?13:32);fail_after=budget;
        try {const auto result=f::DispatchCryptoHashFunction(request);fail_after=-1;
          Check(failure?refuse(result,"CRYPTO.RNG.UNAVAILABLE"):(result.result.ok()&&result.result.scalar_values.size()==1&&result.result.scalar_values[0].binary_value.size()==32),"random-byte allocation sweep reaches truthful success/failure publication");completed=true;
        }catch(const std::bad_alloc&){fail_after=-1;++faults;}
        disarm();
        Check(rng_scratch==nullptr&&(rng_interceptions==0||rng_cleared),"entropy scratch cleared on success and diagnostic allocation unwind");
        Check(request.arguments[0].value.uint64_value==32,"random-byte allocation failure leaves caller count intact");
        if(completed)break;
      }
      allocation_faults+=faults;Check(completed&&faults>0,"random-byte success and failure allocation positions exercised");
    }
  }
  arm(1,16);
  Check(scratchbird::core::FillCryptographicRandomBytes(nullptr,0)&&!scratchbird::core::FillCryptographicRandomBytes(nullptr,1)&&rng_interceptions==0,"Core entropy validates null extents without a provider call");disarm();
  for(unsigned size:{1u,16u,1024u}) {
    std::vector<unsigned char> bytes(size,0x55);arm(0,static_cast<int>(size/2));fail_after=0;
    const bool filled=scratchbird::core::FillCryptographicRandomBytes(bytes.data(),bytes.size());fail_after=-1;disarm();
    Check(!filled&&std::all_of(bytes.begin(),bytes.end(),[](unsigned char ch){return ch==0;}),"shared Core entropy clears entire preexisting destination after partial provider failure without allocating");
  }
}

void CryptoScrypt() {
  const auto package=f::BuildStandardFunctionSeedPackage();
  const auto* entry=package.registry.Lookup("sb.crypto.scrypt");Check(entry!=nullptr,"scrypt has registered seed identity");if(!entry)return;
  f::FunctionCallRequest request;request.context.function_uuid=entry->function_uuid;
  Check(package.registry.BindCallContext(request.context)!=nullptr,"scrypt binds actual binary function identity");
  const auto make=[&](std::string password,std::vector<std::uint8_t> salt,std::uint64_t n,std::uint64_t r,std::uint64_t p,std::uint64_t count) {
    request.arguments={{"password",f::MakeTextValue("character",std::move(password))},{"salt",f::MakeBinaryValue("binary",std::move(salt))},
      {"n",f::MakeUint64Value("uint64",n)},{"r",f::MakeUint64Value("uint32",r)},{"p",f::MakeUint64Value("uint32",p)},{"output_bytes",f::MakeUint64Value("uint16",count)}};
  };
  const auto hex_bytes=[](std::string_view hex) {
    std::vector<std::uint8_t> bytes;auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(std::size_t i=0;i<hex.size();i+=2)bytes.push_back(static_cast<std::uint8_t>(nibble(hex[i])*16+nibble(hex[i+1])));
    return bytes;
  };
  const auto refuse=[](const f::FunctionCallResult& result,const char* code) {
    return !result.result.ok()&&result.result.scalar_values.empty()&&!result.result.diagnostics.empty()&&result.result.diagnostics[0].diagnostic_id==code;
  };
  const auto binary=[](const f::FunctionCallResult& result,std::size_t count) {
    if(!result.result.ok()||result.result.scalar_values.size()!=1)return false;
    const auto& v=result.result.scalar_values[0];
    return !v.is_null&&v.descriptor_id=="binary"&&v.payload_kind==s::SblrValuePayloadKind::binary&&v.binary_value.size()==count&&v.text_value.empty()&&v.encoded_value.empty();
  };
  const auto watch=[&] {
    scrypt_watch=true;scrypt_cleanses=0;scrypt_cleared=false;scrypt_scratch=nullptr;scrypt_workspace_allocation=nullptr;
    scrypt_armed=true;scrypt_return=0;scrypt_prefix=0;scrypt_interceptions=0;
    scrypt_expected_workspace=0;
    if(request.arguments.size()==6) {
      scratchbird::core::crypto::ScryptWorkEstimate cost;
      if(scratchbird::core::crypto::EstimateScryptWork(request.arguments[0].value.text_value.size(),request.arguments[1].value.binary_value.size(),
          request.arguments[2].value.uint64_value,request.arguments[3].value.uint64_value,request.arguments[4].value.uint64_value,request.arguments[5].value.uint64_value,cost)==scratchbird::core::crypto::ScryptEstimateCode::ok)
        scrypt_expected_workspace=static_cast<std::size_t>(cost.workspace_bytes);
    }
  };
  const auto arm=[&](int result,std::size_t prefix) {watch();scrypt_armed=true;scrypt_return=result;scrypt_prefix=prefix;scrypt_interceptions=0;};
  const auto disarm=[] {scrypt_armed=false;scrypt_watch=false;};
  // Fixed independent RFC7914 section12 known answers, not generated by the
  // provider or function implementation under test.
  struct Known {const char* password;const char* salt;std::uint64_t n,r,p;const char* hex;};
  const Known known[]{
    {"","",16,1,1,"77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906"},
    {"password","NaCl",1024,8,16,"fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"},
    {"pleaseletmein","SodiumChloride",16384,8,1,"7023bdcb3afd7348461c06cd81fd38ebfda8fbba904f8e3ea9b543f6545da1f2d5432955613f0fcf62d49705242a9af9e61e85dc0d651e40dfcf017b45575887"}
  };
  for(const auto& v:known) {
    const std::string salt=v.salt;make(v.password,{salt.begin(),salt.end()},v.n,v.r,v.p,64);watch();
    const auto result=f::DispatchCryptoHashFunction(request);disarm();
    Check(binary(result,64)&&result.result.scalar_values[0].binary_value==hex_bytes(v.hex),"actual scrypt execution matches RFC7914 full known answer");
    Check(scrypt_scratch==nullptr&&scrypt_cleanses==1&&scrypt_cleared&&scrypt_interceptions==0,"actual native scrypt clears its entire workspace without an EVP scrypt call");
  }
  // Fixed SHA256 checksums of independently computed Python hashlib.scrypt
  // output for password00-70-e9, salt00-ff-01-00, N16/r1/p1. Python and the
  // product use independent scrypt implementations; the RFC vectors also
  // supply independent algorithm KATs.
  struct Extent {unsigned count;const char* checksum;};
  for(const auto& v:std::array<Extent,7>{{
    {1,"6b23c0d5f35d1b11f9b683f0b0a617355deb11277d91ae091d399c655b87940d"},
    {32,"44247a2a614145831d56b359402d942e8f350f8fde96be2ce4d7710fc414ffc6"},
    {64,"566cd4003b4b7767de424a3dfd602d388c8c3f72d316f102143216259494c1f5"},
    {128,"9a04870a5090f9a28da5478f651c4a55fd3927b6ae8ab80390433bb68aa4af98"},
    {129,"58d97d9700dfd425d146fa7e9aa50d9399dd5fe50884f0cacb37e3e2cd50202e"},
    {1024,"9b4842674d38b31095774ee1f0643e0492d21a5210131b3cd2a60b6bd44f7c11"},
    {65535,"280bcec14a981ac48dbc3a6de65b8164fad046490341f317d41090c301119091"}}}) {
    make(std::string("\0p\xe9",3),{0,255,1,0},16,1,1,v.count);request.arguments[0].value.charset_name="ISO8859_1";
    watch();const auto result=f::DispatchCryptoHashFunction(request);disarm();
    Check(binary(result,v.count)&&scrypt_interceptions==0,"native scrypt publishes requested uint16 output length including above128 bytes");
    if(binary(result,v.count)) {
      std::array<unsigned char,EVP_MAX_MD_SIZE> hash{};unsigned count=0;
      const auto& bytes=result.result.scalar_values[0].binary_value;
      const bool hashed=EVP_Digest(bytes.data(),bytes.size(),hash.data(),&count,EVP_sha256(),nullptr)==1;
      Check(hashed&&count==32&&std::equal(hash.begin(),hash.begin()+32,hex_bytes(v.checksum).begin()),"scrypt preserves embedded NUL/high bytes and the entire independently checksummed output");
    }
  }
  make("",{},16,1,1,64);const auto valid=request.arguments;
  for(unsigned slot=0;slot<6;++slot) {
    request.arguments=valid;request.arguments[slot].value=f::MakeNullValue(valid[slot].value.descriptor_id);arm(0,13);
    const auto result=f::DispatchCryptoHashFunction(request);disarm();
    Check(result.result.ok()&&result.result.scalar_values.size()==1&&result.result.scalar_values[0].is_null&&result.result.scalar_values[0].descriptor_id=="binary"&&scrypt_interceptions==0,"each typed scrypt NULL avoids KDF computation");
  }
  for(unsigned arity:{0u,1u,2u,3u,4u,5u,7u}) {
    request.arguments=valid;request.arguments.resize(arity);arm(0,13);
    Check(refuse(f::DispatchCryptoHashFunction(request),"CRYPTO.PASSWORD.INVALID_PARAMETER")&&scrypt_interceptions==0,"scrypt has no optional argument defaults");disarm();
  }
  for(unsigned slot=0;slot<6;++slot)for(unsigned fault=0;fault<9;++fault) {
    request.arguments=valid;auto& value=request.arguments[slot].value;
    if(fault==0)value.descriptor_id="other";
    if(fault==1)value.has_int64_value=true;
    if(fault==2)value.has_real64_value=true;
    if(fault==3)value.uuid_value=Base();
    if(fault==4)value.payload_kind=s::SblrValuePayloadKind::uuid_binary;
    if(fault==5)value.encoded_value="secret-conflicting-mirror";
    if(fault==6){if(slot==1)value.text_value="secret-salt";else value.binary_value={1};}
    if(fault==7){if(slot<2)value.has_uint64_value=true;else value.has_uint64_value=false;}
    if(fault==8){if(slot==0)value.descriptor_id="binary";else value.charset_name="UTF8";}
    arm(0,13);const auto result=f::DispatchCryptoHashFunction(request);disarm();
    Check(refuse(result,"CRYPTO.PASSWORD.INVALID_PARAMETER")&&scrypt_interceptions==0,"scrypt structural carrier conflicts never invoke provider");
    if(!result.result.diagnostics.empty())Check(result.result.diagnostics[0].detail.find("secret")==std::string::npos,"scrypt rejection diagnostics do not expose secrets");
  }
  struct Invalid {unsigned slot;std::uint64_t value;};
  for(const auto& bad:std::array<Invalid,15>{{{2,0},{2,1},{2,3},{2,65536},{2,~std::uint64_t{0}},{3,0},{4,0},{3,0x100000000ULL},{4,0x100000000ULL},{3,1ULL<<30},{4,1ULL<<30},{5,0},{5,65536},{5,~std::uint64_t{0}},{3,0xffffffffULL}}}) {
    request.arguments=valid;request.arguments[bad.slot].value=f::MakeUint64Value(valid[bad.slot].value.descriptor_id,bad.value);arm(0,13);
    Check(refuse(f::DispatchCryptoHashFunction(request),"CRYPTO.PASSWORD.INVALID_PARAMETER")&&scrypt_interceptions==0,"scrypt RFC/width bounds reject before provider with overflow-safe checks");disarm();
  }
  // Resource costs are rejected before any expensive work. No simulated
  // provider acceptance or failure stands in for execution.
  for(const auto& tuple:std::array<std::array<std::uint64_t,4>,3>{{{1ULL<<31,2,1,64},{1ULL<<63,4,1,64},{2,1,(1ULL<<30)-1,65535}}}) {
    make("",{},tuple[0],tuple[1],tuple[2],tuple[3]);arm(0,13);
    Check(refuse(f::DispatchCryptoHashFunction(request),"RESOURCE.BUDGET_EXCEEDED")&&scrypt_interceptions==0&&scrypt_cleanses==0,
          "structurally valid excessive costs are resource failures, not malformed parameters or provider outages");disarm();
  }
  for(unsigned slot:{0u,1u}) {
    make("",{},16,1,1,64);
    if(slot==0)request.arguments[0].value=f::MakeTextValue("character",std::string(1048577,'x'));
    else request.arguments[1].value=f::MakeBinaryValue("binary",std::vector<std::uint8_t>(1048577,0x55));
    arm(0,13);Check(refuse(f::DispatchCryptoHashFunction(request),"RESOURCE.BUDGET_EXCEEDED")&&scrypt_interceptions==0&&scrypt_cleanses==0,
      "retained input capacity guard is not a claim of invalid password/salt semantics");disarm();
  }
  // Formerly intercepted-only N32768/r1/p1 now executes through the actual
  // function. OpenSSL is an independent test oracle, never its provider.
  make("",{},32768,1,1,64);std::vector<unsigned char> expected(64);const unsigned char empty=0;
  Check(__real_EVP_PBE_scrypt("",0,&empty,0,32768,1,1,32ULL*1024*1024,expected.data(),expected.size())==1,"independent larger-profile oracle executes");
  watch();const auto large=f::DispatchCryptoHashFunction(request);disarm();
  Check(binary(large,64)&&large.result.scalar_values[0].binary_value==expected&&scrypt_interceptions==0,
        "actual function executes larger native profile without external provider or parameter narrowing");
  request.arguments=valid;bool completed=false;unsigned faults=0;
  for(long budget=0;budget<150;++budget) {
    watch();fail_after=budget;
    try {const auto result=f::DispatchCryptoHashFunction(request);fail_after=-1;
      Check(binary(result,64)&&result.result.scalar_values[0].binary_value==hex_bytes(known[0].hex),"native scrypt allocation sweep reaches full known-answer publication");completed=true;
    }catch(const std::bad_alloc&){fail_after=-1;++faults;}
    disarm();Check(scrypt_scratch==nullptr&&(scrypt_cleanses==0||scrypt_cleared)&&scrypt_interceptions==0,"allocation unwind leaves no uncleared native workspace and never invokes EVP scrypt");
    Check(request.arguments[2].value.uint64_value==16,"native allocation failure preserves caller parameters");
    if(completed)break;
  }
  allocation_faults+=faults;Check(completed&&faults>0,"native key/workspace/result allocations exercised through publication");
}

void ScryptCancellation() {
  const auto package=f::BuildStandardFunctionSeedPackage();
  const auto* entry=package.registry.Lookup("sb.crypto.scrypt");Check(entry!=nullptr,"KDF cancellation uses actual scrypt seed");if(!entry)return;
  f::FunctionCallRequest request;request.context.function_uuid=entry->function_uuid;
  Check(package.registry.BindCallContext(request.context)!=nullptr,"KDF cancellation retains binary function binding");
  scratchbird::engine::internal_api::EngineRequestContext owner;
  request.context.engine_request_context=&owner;
  request.arguments={{"password",f::MakeTextValue("character","")},{"salt",f::MakeBinaryValue("binary",{})},
    {"n",f::MakeUint64Value("uint64",16)},{"r",f::MakeUint64Value("uint32",1)},{"p",f::MakeUint64Value("uint32",1)},{"output_bytes",f::MakeUint64Value("uint16",64)}};
  const auto valid=request.arguments;
  constexpr std::string_view expected="77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906";
  const auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(std::size_t i=0;i<64;++i)secret_expected[i]=static_cast<unsigned char>(16*nibble(expected[2*i])+nibble(expected[2*i+1]));
  const auto arm=[] {
    scrypt_armed=true;scrypt_watch=true;scrypt_return=0;scrypt_prefix=0;scrypt_interceptions=0;
    scrypt_expected_workspace=2432;scrypt_scratch=nullptr;scrypt_cleanses=0;scrypt_cleared=false;scrypt_workspace_allocation=nullptr;
    secret_allocation_count=0;prepared_while_workspace_live=false;
    secret_allocations={};secret_tracking_overflow=false;uncleared_secret_frees=0;secret_deallocation_watch=true;
  };
  const auto finish=[] {
    scrypt_armed=false;scrypt_watch=false;secret_deallocation_watch=false;
    Check(!secret_tracking_overflow&&uncleared_secret_frees==0,"no unpublished complete real RFC key is freed without erasure");
    Check(scrypt_scratch==nullptr&&(scrypt_cleanses==0||scrypt_cleared),"function cancellation/exception clears actual native workspace");
    Check(!scrypt_workspace_allocation&&!prepared_while_workspace_live,"native workspace is freed before preparing copied result bytes");
    Check(scrypt_interceptions==0,"function computation and cancellation never invoke EVP scrypt");
  };
  const auto cancelled=[](const f::FunctionCallResult& result) {
    return !result.result.ok()&&result.result.scalar_values.empty()&&!result.result.diagnostics.empty()&&result.result.diagnostics[0].diagnostic_id=="PROCESS.CANCELLED";
  };
  unsigned total_polls=0;owner.query_cancellation_requested=[&]{++total_polls;return false;};
  const auto normal=f::DispatchCryptoHashFunction(request);
  Check(normal.result.ok()&&normal.result.scalar_values.size()==1&&
        normal.result.scalar_values[0].binary_value==std::vector<std::uint8_t>(secret_expected.begin(),secret_expected.end())&&total_polls>4,
        "uncancelled function computes real RFC key with internal cancellation opportunities");
  for(bool null_value:{false,true})for(unsigned stop=1;stop<=(null_value?2:total_polls);++stop) {
    request.arguments=valid;if(null_value)request.arguments[0].value=f::MakeNullValue("character");
    unsigned polls=0;owner.query_cancellation_requested=[&]{return ++polls==stop;};arm();
    {const auto result=f::DispatchCryptoHashFunction(request);
      Check(cancelled(result)&&polls==stop,"every function/computation/final-publication fence honors owning request cancellation");}
    if(null_value||stop==1)Check(scrypt_cleanses==0,"NULL and initial cancellation allocate no native workspace");
    finish();
  }
  struct ProbeFailure {};
  // An independent publication trigger, not a poll-count oracle: request
  // cancellation when the current adapter allocates its prepared key copy.
  // Removing the final function fence must not shorten a self-derived sweep
  // and thereby evade cancellation coverage.
  for(bool throws:{false,true}) {
    request.arguments=valid;
    owner.query_cancellation_requested=[&]{if(secret_allocation_count<2)return false;if(throws)throw ProbeFailure{};return true;};
    arm();bool threw=false;
    try {const auto result=f::DispatchCryptoHashFunction(request);Check(!throws&&cancelled(result),"cancellation after actual key-copy allocation prevents function publication");}
    catch(const ProbeFailure&){threw=true;}
    Check(secret_allocation_count>=2&&threw==throws,"prepared-output cancellation/throw trigger was reached independently of probe count");finish();
  }
  for(unsigned stop=1;stop<=total_polls;++stop) {
    request.arguments=valid;unsigned polls=0;
    owner.query_cancellation_requested=[&]{if(++polls==stop)throw ProbeFailure{};return false;};arm();bool threw=false;
    try {const auto result=f::DispatchCryptoHashFunction(request);(void)result;}catch(const ProbeFailure&){threw=true;}
    Check(threw&&polls==stop,"throwing probes at every native/function fence never approve publication");finish();
  }
  // Fail each C++ allocation through final cancellation diagnostic creation.
  // The native computation still runs, and the observer recognizes its real
  // full RFC key in both original scratch and prepared unpublished output.
  request.arguments=valid;bool completed=false;unsigned faults=0;
  for(long budget=0;budget<180;++budget) {
    unsigned polls=0;owner.query_cancellation_requested=[&]{return ++polls==total_polls;};arm();fail_after=budget;
    try {const auto result=f::DispatchCryptoHashFunction(request);fail_after=-1;
      Check(cancelled(result),"native function allocation sweep reaches final cancellation without scalar publication");completed=true;
    }catch(const std::bad_alloc&){fail_after=-1;++faults;}
    finish();if(completed)break;
  }
  allocation_faults+=faults;Check(completed&&faults>0,"native allocation failures cover workspace, key, prepared output and cancellation diagnostics");
  request.context.engine_request_context=nullptr;
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
  BinaryUuidValues();
  CryptoUuidGeneration();
  CryptoFixedDigests();
  Blake3KnownAnswers();
  CryptoHmac();
  CryptoRandomBytes();
  CryptoScrypt();
  ScryptCancellation();
  std::cout << checks << " checks, " << allocation_faults << " allocation faults, " << failures << " failures\n";
  return failures ? 1 : 0;
}
