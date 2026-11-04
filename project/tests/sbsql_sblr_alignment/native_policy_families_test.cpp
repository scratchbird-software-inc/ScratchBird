// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_NATIVE_POLICY_FAMILY_SEEDS (owner direction 2026-11-04).
// The five native policy families load from the default policy pack's
// policies/native_*.json files. The seeds must equal the values the native
// enrollment fixture used (the drafts were taken from it), and a malformed
// pack must refuse: unknown enum name, dangling reference, duplicate key,
// partial file set. Refusal cases rehash their copied pack, so the loader
// refuses for the stated reason rather than a hash mismatch.
#include "native_policy_families.hpp"
#include "hash_digest.hpp"
#include "../support/owned_temp_directory.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>

namespace {
namespace db = scratchbird::storage::database;
namespace c = scratchbird::core::catalog;
namespace h = scratchbird::core::hash;
unsigned checks = 0;
void Check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }

std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)), {});
  Check(bool(input) || input.eof(), "readable pack file");
  return text;
}
void Write(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
  Check(bool(output), "writable pack file");
}
std::string Sha(const std::string& text) {
  const auto digest = h::ComputeSha256Digest(reinterpret_cast<const scratchbird::core::platform::byte*>(text.data()), text.size());
  Check(digest.ok(), "sha256");
  return h::HexLower(digest.digest);
}
// Recompute every content entry's hash and the aggregate, the way the loader
// checks them (files here are LF-only, so canonicalization is the identity).
void Rehash(const std::filesystem::path& root, bool drop_naming = false) {
  const auto manifest_path = root / "POLICY_PACK_MANIFEST.json";
  auto manifest = Read(manifest_path);
  if (drop_naming) {
    const std::regex entry(",\\s*\\{\\s*\"path\": \"policies/native_naming_profiles.json\",\\s*\"sha256\": \"[0-9a-f]{64}\"\\s*\\}");
    manifest = std::regex_replace(manifest, entry, "");
  }
  const std::regex entry("\"path\": \"([^\"]+)\",\\s*\"sha256\": \"([0-9a-f]{64})\"");
  std::string rebuilt, aggregate;
  std::size_t from = 0;
  for (auto it = std::sregex_iterator(manifest.begin(), manifest.end(), entry); it != std::sregex_iterator(); ++it) {
    const auto& m = *it;
    const auto path = m[1].str();
    const auto sha = Sha(Read(root / path));
    rebuilt += manifest.substr(from, m.position(2) - from) + sha;
    from = m.position(2) + 64;
    aggregate += path + std::string(1, '\0') + sha + "\n";
  }
  rebuilt += manifest.substr(from);
  const std::regex total("\"content_sha256\": \"[0-9a-f]{64}\"");
  rebuilt = std::regex_replace(rebuilt, total, "\"content_sha256\": \"" + Sha(aggregate) + "\"",
                               std::regex_constants::format_first_only);
  Write(manifest_path, rebuilt);
}
// A copy of the default pack with one file edited, rehashed.
db::NativePolicyFamiliesLoadResult LoadEdited(const char* file, const std::string& from, const std::string& to,
                                              bool drop_naming = false) {
  scratchbird::tests::OwnedTempDirectory artifacts;
  const auto root = artifacts.path() / "pack";
  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT, root, std::filesystem::copy_options::recursive);
  if (file) {
    auto text = Read(root / file);
    const auto at = text.find(from);
    Check(at != std::string::npos, "edited text exists in the pack file");
    text.replace(at, from.size(), to);
    Write(root / file, text);
  }
  Rehash(root, drop_naming);
  return db::LoadNativePolicyFamilies(root.string());
}
void Refuses(const db::NativePolicyFamiliesLoadResult& result, const char* why) {
  Check(!result.ok() && !result.seeds &&
            result.diagnostic.diagnostic_code == "SB-POLICY-PACK-NATIVE-FAMILIES-INVALID", why);
}
}  // namespace

int main() try {
  // The default pack: every family present, values as drafted.
  const auto loaded = db::LoadNativePolicyFamilies(SB_DEFAULT_POLICY_PACK_ROOT);
  if (!loaded.ok()) std::cerr << loaded.diagnostic.diagnostic_code << '\n';
  Check(loaded.ok() && loaded.seeds.has_value(), "default pack declares every native policy family");
  const auto& s = *loaded.seeds;
  Check(s.verification_descriptors.size() == 5 && s.verification_bodies.size() == 3 &&
            s.evidence_bodies.size() == 1 && s.authorization_bodies.size() == 1, "verification family shape");
  Check(s.transaction_descriptors.size() == 2 && s.transaction_bodies.size() == 1 && s.lock_bodies.size() == 1 &&
            s.isolation_profiles.size() == 2 && s.profile_bindings.size() == 1, "transaction family shape");
  Check(s.management_profiles.size() == 10 && s.management_descriptors.size() == 1 &&
            s.management_descriptors[0].profiles.size() == 10, "management family shape");
  Check(s.identifier_profiles.size() == 1 && s.dialect_profiles.size() == 1 && s.collision_groups.size() == 1 &&
            s.collision_policies.size() == 1, "naming family shape");
  // Spot values against the enrollment fixture they were drafted from.
  const auto& repair = s.verification_bodies[0];
  Check(repair.role == c::VerificationPolicyRole::repair && repair.verification_class_mask == 511 &&
            repair.authority_class_mask == 63 && repair.verification_level_mask == 1023 &&
            repair.maximum_scan_pages == 4096 && repair.maximum_work_bytes == 128ull * 1024 * 1024 &&
            repair.maximum_findings == 64 && !repair.mark_salvaged_output, "verification repair body values");
  Check(s.verification_bodies[1].access_policy == "verification_access" &&
            s.verification_bodies[2].mark_salvaged_output, "quarantine access policy and salvage flag");
  Check(s.verification_descriptors[0].enforcement_points == 1 &&
            s.verification_descriptors[3].policy_class == c::CatalogPolicyClass::evidence, "verification descriptors");
  Check(s.authorization_bodies[0].maximum_records == 1024 && s.authorization_bodies[0].maximum_work == 262144,
        "authorization body values");
  const auto& tx = s.transaction_bodies[0];
  Check(tx.maximum_active_transactions == 64 && tx.maximum_transaction_microseconds == 60000000 &&
            tx.read_mode_mask == 3 && tx.allowed_isolation_profiles.size() == 2, "transaction body values");
  Check(s.transaction_descriptors[0].enforcement_points == (1ull | (1ull << 7)) &&
            s.transaction_descriptors[0].default_decision == c::CatalogPolicyDefault::allow &&
            s.transaction_descriptors[1].policy_class == c::CatalogPolicyClass::lock_admission &&
            s.transaction_descriptors[1].enforcement_points == (1ull | (1ull << 7)) &&
            s.transaction_descriptors[1].default_decision == c::CatalogPolicyDefault::allow,
        "transaction descriptors: the fixture's default case has no database-scope deny");
  Check(s.lock_bodies[0].maximum_locks == 256 && s.lock_bodies[0].deadlock_detection_interval_microseconds == 100000,
        "lock body values");
  Check(s.isolation_profiles[0].profile_kind == c::IsolationProfileKind::read_committed_statement &&
            s.isolation_profiles[1].profile_kind == c::IsolationProfileKind::maintenance_operation_isolation &&
            s.isolation_profiles[0].snapshot_refresh_mask == 2, "isolation profiles");
  Check(s.management_descriptors[0].code == c::CatalogManagementOperationCode::verify_start &&
            s.management_descriptors[0].flags == 31 &&
            s.management_descriptors[0].profiles == s.management_profiles, "management values");
  // The fixture issues profile codes 1..10 in order, and the descriptor's list
  // must keep that order: a swap is the profile_code_mismatch refusal.
  for (std::size_t i = 0; i < s.management_profiles.size(); ++i)
    Check(static_cast<std::uint64_t>(s.management_profiles[i]) == i + 1, "management profile codes 1..10 in order");
  Check(s.identifier_profiles[0].comparison_mode == c::IdentifierComparisonMode::exact &&
            s.identifier_profiles[0].length_unit == c::IdentifierLengthUnit::encoded_octets &&
            s.collision_policies[0].enforcement_points == (1ull << 13) && s.collision_policies[0].all_built_in_record_kinds &&
            s.collision_groups[0].language_lane == c::CatalogCollisionLanguageLane::exact_tag, "naming and collision values");

  // A rehashed unedited copy still loads, so the refusals below are the edits'.
  const auto unedited = LoadEdited(nullptr, "", "");
  Check(unedited.ok() && unedited.seeds.has_value(), "a rehashed unedited pack copy loads");
  Refuses(LoadEdited("policies/native_verification_policies.json", "\"role\": \"repair\"", "\"role\": \"reconstruct\""),
          "an unknown enum name refuses");
  Refuses(LoadEdited("policies/native_transaction_policies.json", "\"lock_policy\": \"lock_admission\"",
                     "\"lock_policy\": \"no_such_policy\""), "a dangling cross reference refuses");
  Refuses(LoadEdited("policies/native_naming_profiles.json", "\"key\": \"default_dialect\"",
                     "\"key\": \"default_identifier\""), "a duplicate key refuses");
  Refuses(LoadEdited("policies/native_transaction_policies.json", "\"enforcement_points\": [0, 7]",
                     "\"enforcement_points\": [0, 0]"), "a repeated enforcement bit refuses");
  Refuses(LoadEdited(nullptr, "", "", true), "a partial native policy family set refuses");
  std::cout << "PASS native policy families " << checks << " checks\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << "FAIL native policy families: " << e.what() << '\n';
  return 1;
}
