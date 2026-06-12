// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_package.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::storage::filespace {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::UuidToString;

constexpr char kPackageFileMagic[] = "SBFS_PACKAGE_MANIFEST_V1";

Status PackageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_disk};
}

Status PackageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_disk};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.value == right.value;
}

bool IsTypedUuid(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid();
}

std::string DigestString(const std::string& payload) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : payload) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

std::string MemberPayload(const FilespacePackageMember& member) {
  std::ostringstream out;
  out << UuidToString(member.database_uuid.value) << '|'
      << UuidToString(member.filespace_uuid.value) << '|'
      << member.path << '|'
      << FilespaceRoleName(member.role) << '|'
      << FilespaceStateName(member.state) << '|'
      << member.page_size << '|'
      << member.physical_filespace_id << '|'
      << member.header_generation << '|'
      << UuidToString(member.writer_identity_uuid.value);
  return out.str();
}

std::string ManifestPayload(const FilespacePackageManifest& manifest) {
  std::ostringstream out;
  out << UuidToString(manifest.package_uuid.value) << '|'
      << UuidToString(manifest.source_database_uuid.value) << '|'
      << manifest.package_name << '|'
      << manifest.format_version << '|'
      << (manifest.root_authority_present ? "root" : "no_root") << '|'
      << (manifest.encrypted_material_included ? "encrypted" : "no_encrypted_material");
  for (const FilespacePackageMember& member : manifest.members) {
    out << '|' << member.member_checksum;
  }
  return out.str();
}

FilespacePackageResult Error(std::string code,
                             std::string key,
                             std::string detail = {}) {
  FilespacePackageResult result;
  result.status = PackageErrorStatus();
  result.diagnostic =
      MakeFilespaceDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

FilespacePackageFileResult FileError(std::string code,
                                      std::string key,
                                      std::string detail = {},
                                      bool runtime_file_io_executed = false,
                                      u64 byte_count = 0) {
  FilespacePackageFileResult result;
  result.status = PackageErrorStatus();
  result.diagnostic =
      MakeFilespaceDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  result.runtime_package_file_io_executed = runtime_file_io_executed;
  result.byte_count = byte_count;
  return result;
}

FilespacePackageFileResult FileOk(std::string code,
                                   std::string key,
                                   std::string detail,
                                   FilespacePackageManifest manifest,
                                   u64 byte_count) {
  FilespacePackageFileResult result;
  result.status = PackageOkStatus();
  result.diagnostic =
      MakeFilespaceDiagnostic(result.status, std::move(code), std::move(key), std::move(detail));
  result.manifest = std::move(manifest);
  result.byte_count = byte_count;
  result.runtime_package_file_io_executed = true;
  result.physical_package_transfer_executed = false;
  result.encrypted_material_included = false;
  result.durable_state_changed = false;
  result.checksum_verified = true;
  return result;
}

std::filesystem::path PhysicalMemberDirectory(const std::filesystem::path& package_path) {
  const auto filename = package_path.filename().empty()
                            ? std::string("package")
                            : package_path.filename().string();
  return package_path.parent_path() / (filename + ".members");
}

std::filesystem::path PhysicalMemberPath(const std::filesystem::path& package_path,
                                         const FilespacePackageMember& member) {
  return PhysicalMemberDirectory(package_path) /
         (UuidToString(member.filespace_uuid.value) + ".fsp");
}

std::filesystem::path RestoredPhysicalMemberPath(
    const std::filesystem::path& output_directory,
    const FilespacePackageMember& member) {
  return output_directory / (UuidToString(member.filespace_uuid.value) + ".fsp");
}

bool FileDigest(const std::filesystem::path& path, std::string* digest, std::string* detail) {
  if (digest == nullptr) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    if (detail != nullptr) *detail = path.string();
    return false;
  }
  std::uint64_t hash = 1469598103934665603ull;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(buffer[index]));
      hash *= 1099511628211ull;
    }
  }
  if (!input.eof()) {
    if (detail != nullptr) *detail = path.string();
    return false;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  *digest = out.str();
  return true;
}

bool CopyRegularFileVerified(const std::filesystem::path& source,
                             const std::filesystem::path& target,
                             bool allow_overwrite,
                             u64* byte_count,
                             std::string* detail) {
  std::error_code fs_error;
  const auto source_status = std::filesystem::symlink_status(source, fs_error);
  if (fs_error || !std::filesystem::exists(source_status)) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : source.string();
    }
    return false;
  }
  if (!std::filesystem::is_regular_file(source_status)) {
    if (detail != nullptr) *detail = source.string();
    return false;
  }

  const auto parent = target.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, fs_error);
    if (fs_error) {
      if (detail != nullptr) *detail = fs_error.message();
      return false;
    }
  }

  if (std::filesystem::exists(target, fs_error)) {
    if (fs_error) {
      if (detail != nullptr) *detail = fs_error.message();
      return false;
    }
    if (std::filesystem::equivalent(source, target, fs_error) && !fs_error) {
      if (detail != nullptr) *detail = source.string();
      return false;
    }
    if (!allow_overwrite) {
      if (detail != nullptr) *detail = target.string();
      return false;
    }
  }
  fs_error.clear();

  const auto options = allow_overwrite
                           ? std::filesystem::copy_options::overwrite_existing
                           : std::filesystem::copy_options::none;
  if (!std::filesystem::copy_file(source, target, options, fs_error)) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : target.string();
    }
    return false;
  }

  const auto source_size = std::filesystem::file_size(source, fs_error);
  if (fs_error) {
    if (detail != nullptr) *detail = fs_error.message();
    return false;
  }
  const auto target_size = std::filesystem::file_size(target, fs_error);
  if (fs_error || source_size != target_size) {
    if (detail != nullptr) {
      *detail = fs_error ? fs_error.message() : target.string();
    }
    return false;
  }

  std::string source_digest;
  std::string target_digest;
  if (!FileDigest(source, &source_digest, detail) ||
      !FileDigest(target, &target_digest, detail) ||
      source_digest != target_digest) {
    if (detail != nullptr && detail->empty()) {
      *detail = target.string();
    }
    return false;
  }

  if (byte_count != nullptr) {
    *byte_count = static_cast<u64>(source_size);
  }
  return true;
}

FilespacePackageResult Ok(FilespacePackageAction action,
                          FilespacePackageManifest manifest,
                          std::string code,
                          std::string detail = {}) {
  FilespacePackageResult result;
  result.status = PackageOkStatus();
  result.manifest = std::move(manifest);
  result.diagnostic = MakeFilespaceDiagnostic(
      result.status,
      std::move(code),
      std::string("storage.filespace.package.") + FilespacePackageActionName(action),
      std::move(detail));
  return result;
}

std::string HexEncode(const std::string& input) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(input.size() * 2);
  for (const unsigned char ch : input) {
    out.push_back(hex[(ch >> 4) & 0x0f]);
    out.push_back(hex[ch & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

bool HexDecode(const std::string& input, std::string* output) {
  if (output == nullptr || (input.size() % 2) != 0) {
    return false;
  }
  std::string decoded;
  decoded.reserve(input.size() / 2);
  for (std::size_t i = 0; i < input.size(); i += 2) {
    const int high = HexValue(input[i]);
    const int low = HexValue(input[i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    decoded.push_back(static_cast<char>((high << 4) | low));
  }
  *output = std::move(decoded);
  return true;
}

std::string SerializePackageManifestFile(const FilespacePackageManifest& manifest) {
  std::ostringstream out;
  out << kPackageFileMagic << '\n';
  out << "package_uuid=" << UuidToString(manifest.package_uuid.value) << '\n';
  out << "source_database_uuid=" << UuidToString(manifest.source_database_uuid.value) << '\n';
  out << "package_name_hex=" << HexEncode(manifest.package_name) << '\n';
  out << "format_version=" << manifest.format_version << '\n';
  out << "root_authority_present=" << (manifest.root_authority_present ? "1" : "0") << '\n';
  out << "encrypted_material_included="
      << (manifest.encrypted_material_included ? "1" : "0") << '\n';
  out << "member_count=" << manifest.members.size() << '\n';
  for (std::size_t i = 0; i < manifest.members.size(); ++i) {
    const auto prefix = std::string("member.") + std::to_string(i) + ".";
    const FilespacePackageMember& member = manifest.members[i];
    out << prefix << "database_uuid=" << UuidToString(member.database_uuid.value) << '\n';
    out << prefix << "filespace_uuid=" << UuidToString(member.filespace_uuid.value) << '\n';
    out << prefix << "path_hex=" << HexEncode(member.path) << '\n';
    out << prefix << "role=" << FilespaceRoleName(member.role) << '\n';
    out << prefix << "state=" << FilespaceStateName(member.state) << '\n';
    out << prefix << "page_size=" << member.page_size << '\n';
    out << prefix << "physical_filespace_id=" << member.physical_filespace_id << '\n';
    out << prefix << "header_generation=" << member.header_generation << '\n';
    out << prefix << "writer_identity_uuid="
        << UuidToString(member.writer_identity_uuid.value) << '\n';
    out << prefix << "member_checksum=" << member.member_checksum << '\n';
  }
  out << "manifest_checksum=" << manifest.manifest_checksum << '\n';
  out << "END\n";
  return out.str();
}

bool ReadValue(const std::vector<std::string>& lines,
               std::size_t* cursor,
               const std::string& key,
               std::string* value) {
  if (cursor == nullptr || value == nullptr || *cursor >= lines.size()) {
    return false;
  }
  const std::string prefix = key + "=";
  const std::string& line = lines[*cursor];
  if (line.rfind(prefix, 0) != 0) {
    return false;
  }
  *value = line.substr(prefix.size());
  ++(*cursor);
  return true;
}

bool ParseU64Strict(const std::string& text, u64* value) {
  if (value == nullptr || text.empty()) {
    return false;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
      return false;
    }
    *value = static_cast<u64>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseU32Strict(const std::string& text, u32* value) {
  u64 parsed = 0;
  if (!ParseU64Strict(text, &parsed) || parsed > std::numeric_limits<u32>::max()) {
    return false;
  }
  *value = static_cast<u32>(parsed);
  return true;
}

bool ParseU16Strict(const std::string& text, u16* value) {
  u64 parsed = 0;
  if (!ParseU64Strict(text, &parsed) || parsed > std::numeric_limits<u16>::max()) {
    return false;
  }
  *value = static_cast<u16>(parsed);
  return true;
}

bool ParseBool01(const std::string& text, bool* value) {
  if (value == nullptr) {
    return false;
  }
  if (text == "0") {
    *value = false;
    return true;
  }
  if (text == "1") {
    *value = true;
    return true;
  }
  return false;
}

bool ParseRole(const std::string& text, FilespaceRole* role) {
  if (role == nullptr) return false;
#define SB_PARSE_ROLE(value) \
  if (text == FilespaceRoleName(FilespaceRole::value)) { \
    *role = FilespaceRole::value; \
    return true; \
  }
  SB_PARSE_ROLE(unknown)
  SB_PARSE_ROLE(active_primary)
  SB_PARSE_ROLE(primary_shadow)
  SB_PARSE_ROLE(primary_snapshot)
  SB_PARSE_ROLE(primary_candidate)
  SB_PARSE_ROLE(secondary_data)
  SB_PARSE_ROLE(secondary_index)
  SB_PARSE_ROLE(secondary_overflow)
  SB_PARSE_ROLE(secondary_history)
  SB_PARSE_ROLE(secondary_shard)
  SB_PARSE_ROLE(archive_history)
  SB_PARSE_ROLE(archive_log)
  SB_PARSE_ROLE(archive_detached)
  SB_PARSE_ROLE(temporary)
  SB_PARSE_ROLE(import_candidate)
  SB_PARSE_ROLE(drop_pending)
  SB_PARSE_ROLE(forbidden)
#undef SB_PARSE_ROLE
  return false;
}

bool ParseState(const std::string& text, FilespaceState* state) {
  if (state == nullptr) return false;
#define SB_PARSE_STATE(value) \
  if (text == FilespaceStateName(FilespaceState::value)) { \
    *state = FilespaceState::value; \
    return true; \
  }
  SB_PARSE_STATE(absent)
  SB_PARSE_STATE(online)
  SB_PARSE_STATE(read_only)
  SB_PARSE_STATE(detached)
  SB_PARSE_STATE(archived)
  SB_PARSE_STATE(deleted)
  SB_PARSE_STATE(creating)
  SB_PARSE_STATE(initializing)
  SB_PARSE_STATE(maintenance)
  SB_PARSE_STATE(moving)
  SB_PARSE_STATE(relocating_objects)
  SB_PARSE_STATE(promoting)
  SB_PARSE_STATE(demoting)
  SB_PARSE_STATE(detaching)
  SB_PARSE_STATE(drop_pending)
  SB_PARSE_STATE(quarantine)
  SB_PARSE_STATE(forbidden)
#undef SB_PARSE_STATE
  return false;
}

std::vector<std::string> SplitLines(const std::string& content) {
  std::vector<std::string> lines;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

FilespacePackageFileResult ParsePackageManifestFileContent(const std::string& content,
                                                           u64 byte_count) {
  const auto lines = SplitLines(content);
  if (lines.empty() || lines.front() != kPackageFileMagic) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MAGIC-MISMATCH",
                     "storage.filespace.package.file_magic_mismatch",
                     {},
                     true,
                     byte_count);
  }

  FilespacePackageManifest manifest;
  std::size_t cursor = 1;
  std::string value;
  if (!ReadValue(lines, &cursor, "package_uuid", &value)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "package_uuid",
                     true,
                     byte_count);
  }
  const auto package_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::object, value);
  if (!package_uuid.ok()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-UUID-INVALID",
                     "storage.filespace.package.file_uuid_invalid",
                     "package_uuid",
                     true,
                     byte_count);
  }
  manifest.package_uuid = package_uuid.value;

  if (!ReadValue(lines, &cursor, "source_database_uuid", &value)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "source_database_uuid",
                     true,
                     byte_count);
  }
  const auto database_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::database, value);
  if (!database_uuid.ok()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-UUID-INVALID",
                     "storage.filespace.package.file_uuid_invalid",
                     "source_database_uuid",
                     true,
                     byte_count);
  }
  manifest.source_database_uuid = database_uuid.value;

  if (!ReadValue(lines, &cursor, "package_name_hex", &value) ||
      !HexDecode(value, &manifest.package_name)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "package_name_hex",
                     true,
                     byte_count);
  }
  if (!ReadValue(lines, &cursor, "format_version", &value) ||
      !ParseU32Strict(value, &manifest.format_version)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "format_version",
                     true,
                     byte_count);
  }
  if (!ReadValue(lines, &cursor, "root_authority_present", &value) ||
      !ParseBool01(value, &manifest.root_authority_present)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "root_authority_present",
                     true,
                     byte_count);
  }
  if (!ReadValue(lines, &cursor, "encrypted_material_included", &value) ||
      !ParseBool01(value, &manifest.encrypted_material_included)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "encrypted_material_included",
                     true,
                     byte_count);
  }
  u64 member_count = 0;
  if (!ReadValue(lines, &cursor, "member_count", &value) ||
      !ParseU64Strict(value, &member_count) ||
      member_count > 1000000ull) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "member_count",
                     true,
                     byte_count);
  }

  for (u64 i = 0; i < member_count; ++i) {
    const std::string prefix = std::string("member.") + std::to_string(i) + ".";
    FilespacePackageMember member;
    if (!ReadValue(lines, &cursor, prefix + "database_uuid", &value)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "database_uuid",
                       true,
                       byte_count);
    }
    const auto member_database_uuid =
        scratchbird::core::uuid::ParseTypedUuid(UuidKind::database, value);
    if (!member_database_uuid.ok()) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-UUID-INVALID",
                       "storage.filespace.package.file_uuid_invalid",
                       prefix + "database_uuid",
                       true,
                       byte_count);
    }
    member.database_uuid = member_database_uuid.value;

    if (!ReadValue(lines, &cursor, prefix + "filespace_uuid", &value)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "filespace_uuid",
                       true,
                       byte_count);
    }
    const auto filespace_uuid =
        scratchbird::core::uuid::ParseTypedUuid(UuidKind::filespace, value);
    if (!filespace_uuid.ok()) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-UUID-INVALID",
                       "storage.filespace.package.file_uuid_invalid",
                       prefix + "filespace_uuid",
                       true,
                       byte_count);
    }
    member.filespace_uuid = filespace_uuid.value;

    if (!ReadValue(lines, &cursor, prefix + "path_hex", &value) ||
        !HexDecode(value, &member.path)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "path_hex",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "role", &value) ||
        !ParseRole(value, &member.role)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "role",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "state", &value) ||
        !ParseState(value, &member.state)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "state",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "page_size", &value) ||
        !ParseU32Strict(value, &member.page_size)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "page_size",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "physical_filespace_id", &value) ||
        !ParseU16Strict(value, &member.physical_filespace_id)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "physical_filespace_id",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "header_generation", &value) ||
        !ParseU64Strict(value, &member.header_generation)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "header_generation",
                       true,
                       byte_count);
    }
    if (!ReadValue(lines, &cursor, prefix + "writer_identity_uuid", &value)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "writer_identity_uuid",
                       true,
                       byte_count);
    }
    const auto writer_uuid = scratchbird::core::uuid::ParseTypedUuid(UuidKind::object, value);
    if (!writer_uuid.ok()) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-UUID-INVALID",
                       "storage.filespace.package.file_uuid_invalid",
                       prefix + "writer_identity_uuid",
                       true,
                       byte_count);
    }
    member.writer_identity_uuid = writer_uuid.value;

    if (!ReadValue(lines, &cursor, prefix + "member_checksum", &member.member_checksum)) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                       "storage.filespace.package.file_malformed",
                       prefix + "member_checksum",
                       true,
                       byte_count);
    }
    manifest.members.push_back(std::move(member));
  }

  if (!ReadValue(lines, &cursor, "manifest_checksum", &manifest.manifest_checksum)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "manifest_checksum",
                     true,
                     byte_count);
  }
  if (cursor >= lines.size() || lines[cursor] != "END") {
    return FileError("SB-FILESPACE-PACKAGE-FILE-MALFORMED",
                     "storage.filespace.package.file_malformed",
                     "END",
                     true,
                     byte_count);
  }
  ++cursor;
  if (cursor != lines.size()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-TRAILING-CONTENT",
                     "storage.filespace.package.file_trailing_content",
                     {},
                     true,
                     byte_count);
  }

  FilespacePackageRequest inspect_request;
  inspect_request.manifest = manifest;
  const auto inspected = InspectFilespacePackageManifest(inspect_request);
  if (!inspected.ok()) {
    FilespacePackageFileResult result;
    result.status = inspected.status;
    result.diagnostic = inspected.diagnostic;
    result.manifest = std::move(manifest);
    result.byte_count = byte_count;
    result.runtime_package_file_io_executed = true;
    result.physical_package_transfer_executed = false;
    result.encrypted_material_included = false;
    result.durable_state_changed = false;
    result.checksum_verified = false;
    return result;
  }

  return FileOk("SB-FILESPACE-PACKAGE-FILE-READ",
                "storage.filespace.package.file_read",
                std::to_string(member_count),
                std::move(manifest),
                byte_count);
}

bool HasRootAuthority(const FilespaceDescriptor& descriptor) {
  return descriptor.startup_authority ||
         descriptor.catalog_persistence_owner ||
         descriptor.filespace_manifest_owner ||
         descriptor.recovery_evidence_owner ||
         descriptor.first_filespace ||
         descriptor.role == FilespaceRole::active_primary;
}

const FilespaceDescriptor* FindDescriptor(const FilespaceRegistry& registry,
                                          const TypedUuid& filespace_uuid) {
  for (const FilespaceDescriptor& descriptor : registry.filespaces) {
    if (SameUuid(descriptor.filespace_uuid, filespace_uuid)) {
      return &descriptor;
    }
  }
  return nullptr;
}

FilespaceDescriptor* FindMutableDescriptor(FilespaceRegistry* registry,
                                           const TypedUuid& filespace_uuid) {
  if (registry == nullptr) {
    return nullptr;
  }
  for (FilespaceDescriptor& descriptor : registry->filespaces) {
    if (SameUuid(descriptor.filespace_uuid, filespace_uuid)) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool HasDuplicateMemberUuid(const FilespacePackageManifest& manifest) {
  for (std::size_t left = 0; left < manifest.members.size(); ++left) {
    for (std::size_t right = left + 1; right < manifest.members.size(); ++right) {
      if (SameUuid(manifest.members[left].filespace_uuid, manifest.members[right].filespace_uuid)) {
        return true;
      }
    }
  }
  return false;
}

FilespacePackageEvent Event(FilespaceRegistry* registry,
                            FilespacePackageAction action,
                            const TypedUuid& package_uuid,
                            const FilespaceDescriptor& before,
                            const FilespaceDescriptor& after,
                            const char* diagnostic_code,
                            bool durable_state_changed) {
  FilespacePackageEvent event;
  event.sequence = registry == nullptr ? 0 : registry->next_evidence_sequence++;
  event.action = action;
  event.package_uuid = package_uuid;
  event.filespace_uuid = after.filespace_uuid;
  event.previous_state = before.state;
  event.new_state = after.state;
  event.diagnostic_code = diagnostic_code;
  event.durable_state_changed = durable_state_changed;
  return event;
}

FilespacePackageResult ValidateManifest(const FilespacePackageManifest& manifest) {
  if (!IsTypedUuid(manifest.package_uuid, UuidKind::object)) {
    return Error("SB-FILESPACE-PACKAGE-UUID-INVALID",
                 "storage.filespace.package.uuid_invalid");
  }
  if (!IsTypedUuid(manifest.source_database_uuid, UuidKind::database)) {
    return Error("SB-FILESPACE-PACKAGE-SOURCE-DATABASE-UUID-INVALID",
                 "storage.filespace.package.source_database_uuid_invalid");
  }
  if (manifest.format_version != 1) {
    return Error("SB-FILESPACE-PACKAGE-FORMAT-UNSUPPORTED",
                 "storage.filespace.package.format_unsupported",
                 std::to_string(manifest.format_version));
  }
  if (manifest.members.empty()) {
    return Error("SB-FILESPACE-PACKAGE-EMPTY",
                 "storage.filespace.package.empty");
  }
  if (manifest.root_authority_present) {
    return Error("SB-FILESPACE-PACKAGE-ROOT-AUTHORITY-REFUSED",
                 "storage.filespace.package.root_authority_refused");
  }
  if (manifest.encrypted_material_included) {
    return Error("SB-FILESPACE-PACKAGE-ENCRYPTED-MATERIAL-REFUSED",
                 "storage.filespace.package.encrypted_material_refused");
  }
  if (HasDuplicateMemberUuid(manifest)) {
    return Error("SB-FILESPACE-PACKAGE-DUPLICATE-MEMBER",
                 "storage.filespace.package.duplicate_member");
  }
  for (const FilespacePackageMember& member : manifest.members) {
    if (!SameUuid(member.database_uuid, manifest.source_database_uuid)) {
      return Error("SB-FILESPACE-PACKAGE-MEMBER-DATABASE-MISMATCH",
                   "storage.filespace.package.member_database_mismatch");
    }
    if (!IsTypedUuid(member.filespace_uuid, UuidKind::filespace)) {
      return Error("SB-FILESPACE-PACKAGE-MEMBER-FILESPACE-UUID-INVALID",
                   "storage.filespace.package.member_filespace_uuid_invalid");
    }
    if (member.path.empty() || member.header_generation == 0) {
      return Error("SB-FILESPACE-PACKAGE-MEMBER-METADATA-INCOMPLETE",
                   "storage.filespace.package.member_metadata_incomplete");
    }
    if (DigestString(MemberPayload(member)) != member.member_checksum) {
      return Error("SB-FILESPACE-PACKAGE-MEMBER-CHECKSUM-MISMATCH",
                   "storage.filespace.package.member_checksum_mismatch");
    }
  }
  if (DigestString(ManifestPayload(manifest)) != manifest.manifest_checksum) {
    return Error("SB-FILESPACE-PACKAGE-MANIFEST-CHECKSUM-MISMATCH",
                 "storage.filespace.package.manifest_checksum_mismatch");
  }
  return Ok(FilespacePackageAction::inspect_manifest,
            manifest,
            "SB-FILESPACE-PACKAGE-INSPECTED",
            std::to_string(manifest.members.size()));
}

FilespacePackageMember PackageMemberFor(const FilespaceDescriptor& descriptor) {
  FilespacePackageMember member;
  member.database_uuid = descriptor.database_uuid;
  member.filespace_uuid = descriptor.filespace_uuid;
  member.path = descriptor.path;
  member.role = descriptor.role;
  member.state = descriptor.state;
  member.page_size = descriptor.page_size;
  member.physical_filespace_id = descriptor.physical_filespace_id;
  member.header_generation = descriptor.header_generation;
  member.writer_identity_uuid = descriptor.writer_identity_uuid;
  member.member_checksum = DigestString(MemberPayload(member));
  return member;
}

FilespaceDescriptor StagedDescriptorFor(const FilespacePackageMember& member,
                                        const TypedUuid& target_database_uuid) {
  FilespaceDescriptor descriptor;
  descriptor.database_uuid = target_database_uuid;
  descriptor.filespace_uuid = member.filespace_uuid;
  descriptor.path = member.path;
  descriptor.role = FilespaceRole::import_candidate;
  descriptor.state = FilespaceState::quarantine;
  descriptor.page_size = member.page_size;
  descriptor.physical_filespace_id = member.physical_filespace_id;
  descriptor.header_generation = member.header_generation;
  descriptor.writer_identity_uuid = member.writer_identity_uuid;
  descriptor.read_only = true;
  descriptor.active = false;
  return descriptor;
}

}  // namespace

const char* FilespacePackageActionName(FilespacePackageAction action) {
  switch (action) {
    case FilespacePackageAction::export_manifest: return "export_manifest";
    case FilespacePackageAction::inspect_manifest: return "inspect_manifest";
    case FilespacePackageAction::import_to_quarantine: return "import_to_quarantine";
    case FilespacePackageAction::admit: return "admit";
    case FilespacePackageAction::reject: return "reject";
  }
  return "unknown";
}

FilespacePackageResult ExportFilespacePackageManifest(
    const FilespacePackageRequest& request) {
  if (!IsTypedUuid(request.package_uuid, UuidKind::object)) {
    return Error("SB-FILESPACE-PACKAGE-UUID-INVALID",
                 "storage.filespace.package.uuid_invalid");
  }
  if (!IsTypedUuid(request.database_uuid, UuidKind::database)) {
    return Error("SB-FILESPACE-PACKAGE-DATABASE-UUID-INVALID",
                 "storage.filespace.package.database_uuid_invalid");
  }
  if (request.descriptors.empty()) {
    return Error("SB-FILESPACE-PACKAGE-EXPORT-EMPTY",
                 "storage.filespace.package.export_empty");
  }

  FilespacePackageManifest manifest;
  manifest.package_uuid = request.package_uuid;
  manifest.source_database_uuid = request.database_uuid;
  manifest.package_name = request.package_name.empty() ? "filespace_package" : request.package_name;
  for (const FilespaceDescriptor& descriptor : request.descriptors) {
    if (!SameUuid(descriptor.database_uuid, request.database_uuid)) {
      return Error("SB-FILESPACE-PACKAGE-EXPORT-DATABASE-MISMATCH",
                   "storage.filespace.package.export_database_mismatch");
    }
    if (HasRootAuthority(descriptor)) {
      return Error("SB-FILESPACE-PACKAGE-ROOT-AUTHORITY-REFUSED",
                   "storage.filespace.package.root_authority_refused");
    }
    manifest.members.push_back(PackageMemberFor(descriptor));
  }
  if (HasDuplicateMemberUuid(manifest)) {
    return Error("SB-FILESPACE-PACKAGE-DUPLICATE-MEMBER",
                 "storage.filespace.package.duplicate_member");
  }
  manifest.manifest_checksum = DigestString(ManifestPayload(manifest));
  return Ok(FilespacePackageAction::export_manifest,
            std::move(manifest),
            "SB-FILESPACE-PACKAGE-EXPORTED",
            std::to_string(request.descriptors.size()));
}

FilespacePackageResult InspectFilespacePackageManifest(
    const FilespacePackageRequest& request) {
  return ValidateManifest(request.manifest);
}

FilespacePackageFileResult WriteFilespacePackageFile(
    const FilespacePackageFileWriteRequest& request) {
  if (request.path.empty()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-PATH-REQUIRED",
                     "storage.filespace.package.file_path_required");
  }

  FilespacePackageRequest inspect_request;
  inspect_request.manifest = request.manifest;
  const auto inspected = InspectFilespacePackageManifest(inspect_request);
  if (!inspected.ok()) {
    FilespacePackageFileResult result;
    result.status = inspected.status;
    result.diagnostic = inspected.diagnostic;
    result.manifest = request.manifest;
    result.physical_package_transfer_executed = false;
    result.encrypted_material_included = request.manifest.encrypted_material_included;
    result.durable_state_changed = false;
    result.checksum_verified = false;
    return result;
  }

  std::error_code fs_error;
  const auto parent = request.path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, fs_error);
    if (fs_error) {
      return FileError("SB-FILESPACE-PACKAGE-FILE-PARENT-CREATE-FAILED",
                       "storage.filespace.package.file_parent_create_failed",
                       fs_error.message());
    }
  }
  if (!request.allow_overwrite && std::filesystem::exists(request.path, fs_error)) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-EXISTS",
                     "storage.filespace.package.file_exists",
                     request.path.string());
  }
  if (fs_error) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-STAT-FAILED",
                     "storage.filespace.package.file_stat_failed",
                     fs_error.message());
  }

  const std::string payload = SerializePackageManifestFile(request.manifest);
  std::ofstream out(request.path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-WRITE-FAILED",
                     "storage.filespace.package.file_write_failed",
                     request.path.string());
  }
  out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  out.flush();
  const bool flushed = static_cast<bool>(out);
  out.close();
  if (!flushed || !out) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-WRITE-FAILED",
                     "storage.filespace.package.file_write_failed",
                     request.path.string(),
                     true,
                     static_cast<u64>(payload.size()));
  }

  auto result = FileOk("SB-FILESPACE-PACKAGE-FILE-WRITTEN",
                       "storage.filespace.package.file_written",
                       request.path.string(),
                       request.manifest,
                       static_cast<u64>(payload.size()));
  result.file_flushed = true;
  result.filesystem_sync_executed = false;
  if (request.execute_physical_package_transfer) {
    if (!request.allow_physical_package_transfer) {
      return FileError("SB-FILESPACE-PACKAGE-PHYSICAL-TRANSFER-FORBIDDEN",
                       "storage.filespace.package.physical_transfer_forbidden",
                       request.path.string(),
                       true,
                       static_cast<u64>(payload.size()));
    }
    u64 physical_bytes = 0;
    for (const FilespacePackageMember& member : request.manifest.members) {
      u64 copied_bytes = 0;
      std::string detail;
      if (!CopyRegularFileVerified(member.path,
                                   PhysicalMemberPath(request.path, member),
                                   request.allow_overwrite,
                                   &copied_bytes,
                                   &detail)) {
        return FileError("SB-FILESPACE-PACKAGE-PHYSICAL-TRANSFER-FAILED",
                         "storage.filespace.package.physical_transfer_failed",
                         detail,
                         true,
                         static_cast<u64>(payload.size()));
      }
      physical_bytes += copied_bytes;
      ++result.physical_member_count;
    }
    result.physical_package_transfer_executed = true;
    result.physical_byte_count = physical_bytes;
  }
  return result;
}

FilespacePackageFileResult ReadFilespacePackageFile(
    const FilespacePackageFileReadRequest& request) {
  if (request.path.empty()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-PATH-REQUIRED",
                     "storage.filespace.package.file_path_required");
  }
  std::ifstream in(request.path, std::ios::binary);
  if (!in.is_open()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-READ-FAILED",
                     "storage.filespace.package.file_read_failed",
                     request.path.string(),
                     true);
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return FileError("SB-FILESPACE-PACKAGE-FILE-READ-FAILED",
                     "storage.filespace.package.file_read_failed",
                     request.path.string(),
                     true);
  }
  const std::string content = buffer.str();
  auto result = ParsePackageManifestFileContent(content, static_cast<u64>(content.size()));
  if (!result.ok() || !request.execute_physical_package_transfer) {
    return result;
  }
  if (!request.allow_physical_package_transfer) {
    return FileError("SB-FILESPACE-PACKAGE-PHYSICAL-TRANSFER-FORBIDDEN",
                     "storage.filespace.package.physical_transfer_forbidden",
                     request.path.string(),
                     true,
                     static_cast<u64>(content.size()));
  }
  if (request.physical_output_directory.empty()) {
    return FileError("SB-FILESPACE-PACKAGE-PHYSICAL-OUTPUT-REQUIRED",
                     "storage.filespace.package.physical_output_required",
                     request.path.string(),
                     true,
                     static_cast<u64>(content.size()));
  }

  u64 physical_bytes = 0;
  for (const FilespacePackageMember& member : result.manifest.members) {
    u64 copied_bytes = 0;
    std::string detail;
    if (!CopyRegularFileVerified(PhysicalMemberPath(request.path, member),
                                 RestoredPhysicalMemberPath(request.physical_output_directory,
                                                            member),
                                 true,
                                 &copied_bytes,
                                 &detail)) {
      return FileError("SB-FILESPACE-PACKAGE-PHYSICAL-TRANSFER-FAILED",
                       "storage.filespace.package.physical_transfer_failed",
                       detail,
                       true,
                       static_cast<u64>(content.size()));
    }
    physical_bytes += copied_bytes;
    ++result.physical_member_count;
  }
  result.physical_package_transfer_executed = true;
  result.physical_byte_count = physical_bytes;
  return result;
}

FilespacePackageResult ImportFilespacePackageToQuarantine(
    FilespaceRegistry* registry,
    const FilespacePackageRequest& request) {
  if (registry == nullptr) {
    return Error("SB-FILESPACE-PACKAGE-REGISTRY-NULL",
                 "storage.filespace.package.registry_null");
  }
  if (!request.inspection_passed) {
    return Error("SB-FILESPACE-PACKAGE-IMPORT-INSPECTION-REQUIRED",
                 "storage.filespace.package.import_inspection_required");
  }
  if (!IsTypedUuid(request.target_database_uuid, UuidKind::database)) {
    return Error("SB-FILESPACE-PACKAGE-TARGET-DATABASE-UUID-INVALID",
                 "storage.filespace.package.target_database_uuid_invalid");
  }
  auto inspected = ValidateManifest(request.manifest);
  if (!inspected.ok()) {
    return inspected;
  }
  for (const FilespacePackageMember& member : request.manifest.members) {
    if (FindDescriptor(*registry, member.filespace_uuid) != nullptr) {
      return Error("SB-FILESPACE-PACKAGE-IMPORT-DUPLICATE-FILESPACE",
                   "storage.filespace.package.import_duplicate_filespace");
    }
  }

  FilespacePackageResult result =
      Ok(FilespacePackageAction::import_to_quarantine,
         request.manifest,
         "SB-FILESPACE-PACKAGE-IMPORTED-TO-QUARANTINE",
         std::to_string(request.manifest.members.size()));
  for (const FilespacePackageMember& member : request.manifest.members) {
    const auto staged = StagedDescriptorFor(member, request.target_database_uuid);
    registry->filespaces.push_back(staged);
    result.events.push_back(Event(registry,
                                  FilespacePackageAction::import_to_quarantine,
                                  request.manifest.package_uuid,
                                  FilespaceDescriptor{},
                                  staged,
                                  "SB-FILESPACE-PACKAGE-IMPORTED-TO-QUARANTINE",
                                  true));
    ++result.staged_count;
  }
  result.durable_state_changed = result.staged_count != 0;
  result.cache_invalidation_required = result.durable_state_changed;
  return result;
}

FilespacePackageResult AdmitFilespacePackage(FilespaceRegistry* registry,
                                             const FilespacePackageRequest& request) {
  if (registry == nullptr) {
    return Error("SB-FILESPACE-PACKAGE-REGISTRY-NULL",
                 "storage.filespace.package.registry_null");
  }
  if (!request.inspection_passed || !request.admission_authorized ||
      request.operator_identity.empty()) {
    return Error("SB-FILESPACE-PACKAGE-ADMIT-AUTHORITY-REQUIRED",
                 "storage.filespace.package.admit_authority_required");
  }
  auto inspected = ValidateManifest(request.manifest);
  if (!inspected.ok()) {
    return inspected;
  }

  FilespacePackageResult result =
      Ok(FilespacePackageAction::admit,
         request.manifest,
         "SB-FILESPACE-PACKAGE-ADMITTED",
         request.operator_identity);
  for (const FilespacePackageMember& member : request.manifest.members) {
    FilespaceDescriptor* descriptor = FindMutableDescriptor(registry, member.filespace_uuid);
    if (descriptor == nullptr ||
        descriptor->state != FilespaceState::quarantine ||
        descriptor->role != FilespaceRole::import_candidate) {
      return Error("SB-FILESPACE-PACKAGE-ADMIT-STAGED-CANDIDATE-REQUIRED",
                   "storage.filespace.package.admit_staged_candidate_required");
    }
    const auto before = *descriptor;
    descriptor->state = FilespaceState::detached;
    descriptor->role = FilespaceRole::secondary_data;
    descriptor->read_only = false;
    descriptor->active = false;
    descriptor->startup_authority = false;
    descriptor->catalog_persistence_owner = false;
    descriptor->filespace_manifest_owner = false;
    descriptor->recovery_evidence_owner = false;
    descriptor->first_filespace = false;
    result.events.push_back(Event(registry,
                                  FilespacePackageAction::admit,
                                  request.manifest.package_uuid,
                                  before,
                                  *descriptor,
                                  "SB-FILESPACE-PACKAGE-ADMITTED",
                                  true));
    ++result.admitted_count;
  }
  result.durable_state_changed = result.admitted_count != 0;
  result.cache_invalidation_required = result.durable_state_changed;
  return result;
}

FilespacePackageResult RejectFilespacePackage(FilespaceRegistry* registry,
                                              const FilespacePackageRequest& request) {
  if (registry == nullptr) {
    return Error("SB-FILESPACE-PACKAGE-REGISTRY-NULL",
                 "storage.filespace.package.registry_null");
  }
  if (!request.inspection_passed || !request.reject_authorized ||
      request.operator_identity.empty()) {
    return Error("SB-FILESPACE-PACKAGE-REJECT-AUTHORITY-REQUIRED",
                 "storage.filespace.package.reject_authority_required");
  }
  auto inspected = ValidateManifest(request.manifest);
  if (!inspected.ok()) {
    return inspected;
  }

  FilespacePackageResult result =
      Ok(FilespacePackageAction::reject,
         request.manifest,
         "SB-FILESPACE-PACKAGE-REJECTED",
         request.operator_identity);
  for (const FilespacePackageMember& member : request.manifest.members) {
    FilespaceDescriptor* descriptor = FindMutableDescriptor(registry, member.filespace_uuid);
    if (descriptor == nullptr ||
        descriptor->state != FilespaceState::quarantine ||
        descriptor->role != FilespaceRole::import_candidate) {
      return Error("SB-FILESPACE-PACKAGE-REJECT-STAGED-CANDIDATE-REQUIRED",
                   "storage.filespace.package.reject_staged_candidate_required");
    }
    const auto before = *descriptor;
    descriptor->state = FilespaceState::deleted;
    descriptor->role = FilespaceRole::archive_detached;
    descriptor->read_only = true;
    descriptor->active = false;
    result.events.push_back(Event(registry,
                                  FilespacePackageAction::reject,
                                  request.manifest.package_uuid,
                                  before,
                                  *descriptor,
                                  "SB-FILESPACE-PACKAGE-REJECTED",
                                  true));
    ++result.rejected_count;
  }
  result.durable_state_changed = result.rejected_count != 0;
  result.cache_invalidation_required = result.durable_state_changed;
  return result;
}

}  // namespace scratchbird::storage::filespace
