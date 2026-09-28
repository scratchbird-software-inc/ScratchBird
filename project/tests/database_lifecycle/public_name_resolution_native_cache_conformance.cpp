// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/public_name_resolution_cache.hpp"
#include "../../src/server/public_name_resolution_trace.hpp"
#include <cstdlib>
#include <iostream>
#include <map>
#include <type_traits>

namespace server = scratchbird::server;
namespace trace = server::name_resolution_trace;
namespace packet = scratchbird::wire::public_result;
using Key = server::ServerPublicNameResolutionCacheKey;
using Uuid = scratchbird::core::platform::Uuid;
#define CHECK(c) do { if (!(c)) { std::cerr << "native name-cache check failed at " << __LINE__ << '\n'; std::abort(); } } while(false)
static_assert(std::is_same_v<decltype(Key::database_uuid), Uuid>);
static_assert(std::is_same_v<decltype(Key::effective_user_uuid), Uuid>);
static_assert(std::is_same_v<decltype(Key::dialect_profile_uuid), Uuid>);
static_assert(sizeof(Uuid) == 16);

Key Inputs() {
  Key input;
  input.database_uuid.bytes = {1,0,10,13,124,9,0x70,0,0x80,0,58,59,44,61,0,1};
  input.effective_user_uuid = input.database_uuid; input.effective_user_uuid.bytes[15] = 2;
  input.dialect_profile_uuid = input.database_uuid; input.dialect_profile_uuid.bytes[15] = 3;
  input.presented_name = "public.item";
  input.object_class = "relation"; input.identifier_profile = "sbsql_v3";
  input.language = "en"; input.search_path = "sys,public";
  input.catalog_generation = 11; input.security_epoch = 12;
  input.descriptor_epoch = 13; input.grant_epoch = 14; input.policy_generation = 15;
  input.name_resolution_epoch = 16; input.language_resource_epoch = 17;
  input.localized_name_epoch = 18; input.message_resource_epoch = 19;
  input.role_set_hash = "role"; input.group_set_hash = "group";
  input.search_path_hash = "path"; input.language_profile = "language";
  input.language_tag = "en"; input.input_syntax_profile = "sbsql.syntax.standard";
  input.input_language_fallback_tag = "en"; input.common_resource_hash = "common";
  input.resource_compatibility_identity = "compatible";
  input.resource_version_identity = "version";
  return input;
}
Key Build(const Key& input, bool stable) {
  return server::MakeServerPublicNameResolutionCacheKey(input,
      input.presented_name, input.quoted, input.dialect_profile_uuid,
      input.identifier_profile, input.language, input.search_path,
      input.object_class, stable);
}
bool SystemIdentity(const Uuid& id) {
  return (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
}
int main() {
  const auto input = Inputs();
  const auto normal = Build(input, false), stable = Build(input, true);
  CHECK(normal.HasValidIdentity() && stable.HasValidIdentity() && normal != stable);
  CHECK(normal.database_uuid == input.database_uuid &&
        normal.effective_user_uuid == input.effective_user_uuid &&
        normal.dialect_profile_uuid == input.dialect_profile_uuid);
  CHECK(normal.search_path == input.search_path && normal.search_path_hash == input.search_path_hash);
  CHECK(stable.qualified && stable.search_path.empty() && stable.search_path_hash.empty());
  CHECK(stable.catalog_generation == 0 && stable.descriptor_epoch == 0 &&
        stable.name_resolution_epoch == 0);
  std::size_t octets = 0;
  for (auto member : {&Key::database_uuid, &Key::effective_user_uuid, &Key::dialect_profile_uuid}) {
    for (std::size_t position = 0; position != 16; ++position) {
      for (unsigned value = 0; value != 256; ++value) {
        auto changed = input; (changed.*member).bytes[position] = value;
        const auto key = Build(changed, false);
        CHECK(key.*member == changed.*member);
        CHECK(key.HasValidIdentity() == SystemIdentity(changed.*member));
        CHECK((key == normal) == ((changed.*member) == (input.*member)));
        std::map<Key, Uuid> retained{{normal, input.database_uuid}};
        retained[key] = input.effective_user_uuid;
        CHECK(retained.size() == (key == normal ? 1u : 2u));
        CHECK(retained.at(key) == input.effective_user_uuid);
        const auto encoded = server::PublicNameResolutionCacheKeyTrace(key);
        const char* name = member == &Key::database_uuid ? "database_uuid" :
                           member == &Key::effective_user_uuid ? "effective_user_uuid" : "dialect_profile_uuid";
        const auto field = packet::Find(encoded, name);
        CHECK(field && field->kind == packet::Kind::uuid && field->value.size() == 16);
        CHECK(std::equal((changed.*member).bytes.begin(), (changed.*member).bytes.end(),
            reinterpret_cast<const std::uint8_t*>(field->value.data())));
        ++octets;
      }
    }
    auto invalid = input; invalid.*member = {};
    CHECK(!Build(invalid, false).HasValidIdentity());
  }
  for (auto member : {&Key::catalog_generation, &Key::descriptor_epoch, &Key::name_resolution_epoch}) {
    auto changed = input; changed.*member = UINT64_MAX;
    CHECK(Build(changed, false) != normal && Build(changed, true) == stable);
  }
  for (auto member : {&Key::security_epoch, &Key::grant_epoch, &Key::policy_generation,
                     &Key::language_resource_epoch, &Key::localized_name_epoch, &Key::message_resource_epoch}) {
    auto changed = input; changed.*member = UINT64_MAX;
    CHECK(Build(changed, false) != normal && Build(changed, true) != stable);
  }
  for (auto member : {&Key::presented_name, &Key::object_class, &Key::identifier_profile,
                     &Key::language, &Key::role_set_hash, &Key::group_set_hash,
                     &Key::language_profile, &Key::language_tag, &Key::input_syntax_profile,
                     &Key::input_language_fallback_tag, &Key::common_resource_hash,
                     &Key::resource_compatibility_identity, &Key::resource_version_identity}) {
    auto changed = input; (changed.*member).append("\0|;=\xff", 5);
    CHECK(Build(changed, false) != normal && Build(changed, true) != stable);
  }
  for (auto member : {&Key::search_path, &Key::search_path_hash}) {
    auto changed = input; changed.*member = "<qualified>";
    CHECK(Build(changed, false) != normal && Build(changed, true) == stable);
    changed.presented_name = "item";
    auto unqualified = input; unqualified.presented_name = "item";
    CHECK(Build(changed, true) != Build(unqualified, true));
  }
  auto quoted = input; quoted.quoted = true;
  CHECK(Build(quoted, false) != normal && Build(quoted, true) != stable);
  // Former delimiter joins cannot alias distinct label tuples.
  auto left = input, right = input;
  left.role_set_hash = "a|group_hash=b"; left.group_set_hash = "c";
  right.role_set_hash = "a"; right.group_set_hash = "b|group_hash=c";
  CHECK(Build(left, false) != Build(right, false));
  CHECK(Build(left, true) != Build(right, true));

  const auto key_bytes = server::PublicNameResolutionCacheKeyTrace(normal);
  const auto frame = trace::Encode({
      trace::Text("format", "ps.name.resolution.trace.v3"),
      trace::Identity("object_uuid", input.database_uuid),
      trace::Number("elapsed_us", UINT64_MAX),
      {"cache_key", packet::Kind::row, key_bytes}});
  CHECK(frame.starts_with("SBNRT003"));
  std::uint64_t size = 0;
  for (unsigned n = 0; n != 8; ++n)
    size |= std::uint64_t(static_cast<unsigned char>(frame[8+n])) << (n * 8);
  CHECK(size == frame.size() - 48);
  std::vector<packet::Field> decoded;
  CHECK(trace::Decode(frame, &decoded) && decoded.size() == 4);
  CHECK(decoded[1].kind == packet::Kind::uuid && decoded[1].value.size() == 16);
  CHECK(packet::AsUnsigned(decoded[2]) == UINT64_MAX);
  CHECK(decoded[3].kind == packet::Kind::row && decoded[3].value == key_bytes);
  const auto refuses = [&](std::string_view bad) {
    std::vector<packet::Field> untouched{{"sentinel", packet::Kind::text, "retained"}};
    CHECK(!trace::Decode(bad, &untouched));
    CHECK(untouched.size() == 1 && untouched[0].name == "sentinel" && untouched[0].value == "retained");
  };
  for (std::size_t n = 0; n != frame.size(); ++n) {
    refuses(std::string_view(frame).substr(0, n));
    auto corrupt = frame; corrupt[n] ^= 1; refuses(corrupt);
  }
  refuses(frame + "x");
  auto oversized = frame; for (unsigned n = 0; n != 8; ++n) oversized[8+n] = char(0xff);
  refuses(oversized);
  CHECK(trace::Encode({{"id", packet::Kind::uuid, std::string(15, 'x')}}).empty());
  CHECK(trace::Encode({{"id", packet::Kind::uuid, std::string(17, 'x')}}).empty());
  CHECK(trace::Encode({{"id", packet::Kind::uuid, "019f0900-0000-7000-8000-000000002b01"}}).empty());
  std::cout << "native_name_resolution_cache=pass octet_cases=" << octets << '\n';
}
