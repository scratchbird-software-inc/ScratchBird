// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_os_authority.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <accctrl.h>
#include <lm.h>
#else
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace scratchbird::cli {
namespace {

std::string Trim(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool SafeProfileToken(const std::string& value) {
  if (value.empty() || value.size() > 255 || value == "operator_required") {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
  });
}

bool SafeServiceIdentityToken(const std::string& value) {
  if (!SafeProfileToken(value)) return false;
  const std::string lowered = LowerAscii(value);
  return lowered != "root" && lowered != "administrator" &&
         lowered != "system" && lowered != "localsystem";
}

constexpr const char* kPosixScratchBirdServiceIdentity = "scratchbird";
constexpr const char* kPosixScratchBirdServiceGroup = "scratchbird";
constexpr const char* kWindowsScratchBirdServiceIdentity =
    "NT SERVICE\\scratchbird";
constexpr const char* kWindowsScratchBirdServiceGroup = "ScratchBird";

bool PlatformProfileIdentityContractValid(
    const BootstrapPlatformProfile& profile) {
  const std::string platform = LowerAscii(profile.platform);
  if (platform == "windows") {
    return profile.service_identity == kWindowsScratchBirdServiceIdentity &&
           profile.service_group == kWindowsScratchBirdServiceGroup;
  }
  if (platform != "linux" && platform != "macos" && platform != "posix") {
    return false;
  }
  return profile.service_identity == kPosixScratchBirdServiceIdentity &&
         profile.service_group == kPosixScratchBirdServiceGroup &&
         SafeServiceIdentityToken(profile.service_identity) &&
         SafeProfileToken(profile.service_group);
}

std::string CompiledPlatform() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "posix";
#endif
}

BootstrapOsAuthorityResult Denied(std::string detail) {
  BootstrapOsAuthorityResult result;
  result.protected_audit_detail = std::move(detail);
  return result;
}

#ifdef _WIN32

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) return {};
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), out.data(), length) !=
      length) {
    return {};
  }
  return out;
}

bool TokenMembership(PSID sid) {
  BOOL member = FALSE;
  return CheckTokenMembership(nullptr, sid, &member) != 0 && member != FALSE;
}

bool CurrentTokenIsAdministrator() {
  BYTE storage[SECURITY_MAX_SID_SIZE]{};
  DWORD size = sizeof(storage);
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, storage,
                          &size)) {
    return false;
  }
  return TokenMembership(storage);
}

bool IsManagedServiceSid(PSID sid) {
  if (sid == nullptr || IsValidSid(sid) == FALSE) return false;
  const SID_IDENTIFIER_AUTHORITY expected = SECURITY_NT_AUTHORITY;
  const auto* actual = GetSidIdentifierAuthority(sid);
  if (actual == nullptr ||
      !std::equal(std::begin(actual->Value), std::end(actual->Value),
                  std::begin(expected.Value))) {
    return false;
  }
  const auto* count = GetSidSubAuthorityCount(sid);
  if (count == nullptr || *count != 6) return false;
  const auto* base_rid = GetSidSubAuthority(sid, 0);
  return base_rid != nullptr && *base_rid == 80;
}

bool LookupIntendedManagedServiceIdentity(
    const std::string& identity_name,
    std::vector<unsigned char>* sid) {
  if (sid == nullptr ||
      identity_name != kWindowsScratchBirdServiceIdentity) {
    return false;
  }
  const std::wstring qualified = Utf8ToWide(identity_name);
  if (qualified != L"NT SERVICE\\scratchbird") return false;
  DWORD sid_size = 0;
  DWORD domain_size = 0;
  SID_NAME_USE use = SidTypeUnknown;
  (void)LookupAccountNameW(nullptr, qualified.c_str(), nullptr, &sid_size,
                           nullptr, &domain_size, &use);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_size == 0 ||
      domain_size == 0) {
    return false;
  }
  sid->assign(sid_size, 0);
  std::vector<wchar_t> domain(domain_size, L'\0');
  if (!LookupAccountNameW(nullptr, qualified.c_str(), sid->data(), &sid_size,
                          domain.data(), &domain_size, &use)) {
    return false;
  }
  return use == SidTypeUser &&
         std::wstring(domain.data()) == L"NT SERVICE" &&
         IsManagedServiceSid(sid->data());
}

bool ManagedServiceSidHasNoExplicitLocalGroupMembership(PSID service_sid) {
  if (service_sid == nullptr || IsValidSid(service_sid) == FALSE) return false;
  std::vector<bool> explicit_membership_rows;
  DWORD_PTR group_resume = 0;
  for (;;) {
    LPBYTE group_buffer = nullptr;
    DWORD groups_read = 0;
    DWORD groups_total = 0;
    const NET_API_STATUS group_status = NetLocalGroupEnum(
        nullptr, 0, &group_buffer, MAX_PREFERRED_LENGTH, &groups_read,
        &groups_total, &group_resume);
    if (group_status != NERR_Success && group_status != ERROR_MORE_DATA) {
      if (group_buffer != nullptr) NetApiBufferFree(group_buffer);
      return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
          false, explicit_membership_rows);
    }
    if (groups_read > 0 && group_buffer == nullptr) {
      return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
          false, explicit_membership_rows);
    }
    const auto* groups =
        reinterpret_cast<const LOCALGROUP_INFO_0*>(group_buffer);
    for (DWORD group_index = 0; group_index < groups_read; ++group_index) {
      if (groups[group_index].lgrpi0_name == nullptr ||
          groups[group_index].lgrpi0_name[0] == L'\0') {
        NetApiBufferFree(group_buffer);
        return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
            false, explicit_membership_rows);
      }
      DWORD_PTR member_resume = 0;
      for (;;) {
        LPBYTE member_buffer = nullptr;
        DWORD members_read = 0;
        DWORD members_total = 0;
        const NET_API_STATUS member_status = NetLocalGroupGetMembers(
            nullptr, groups[group_index].lgrpi0_name, 0, &member_buffer,
            MAX_PREFERRED_LENGTH, &members_read, &members_total,
            &member_resume);
        if (member_status != NERR_Success &&
            member_status != ERROR_MORE_DATA) {
          if (member_buffer != nullptr) NetApiBufferFree(member_buffer);
          NetApiBufferFree(group_buffer);
          return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
              false, explicit_membership_rows);
        }
        if (members_read > 0 && member_buffer == nullptr) {
          NetApiBufferFree(group_buffer);
          return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
              false, explicit_membership_rows);
        }
        const auto* members = reinterpret_cast<
            const LOCALGROUP_MEMBERS_INFO_0*>(member_buffer);
        for (DWORD member_index = 0; member_index < members_read;
             ++member_index) {
          if (members[member_index].lgrmi0_sid == nullptr ||
              IsValidSid(members[member_index].lgrmi0_sid) == FALSE) {
            if (member_buffer != nullptr) NetApiBufferFree(member_buffer);
            NetApiBufferFree(group_buffer);
            return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
                false, explicit_membership_rows);
          }
          explicit_membership_rows.push_back(
              EqualSid(members[member_index].lgrmi0_sid, service_sid) != FALSE);
        }
        if (member_buffer != nullptr) NetApiBufferFree(member_buffer);
        if (member_status == NERR_Success) break;
        if (members_read == 0) {
          NetApiBufferFree(group_buffer);
          return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
              false, explicit_membership_rows);
        }
      }
    }
    if (group_buffer != nullptr) NetApiBufferFree(group_buffer);
    if (group_status == NERR_Success) break;
    if (groups_read == 0) {
      return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
          false, explicit_membership_rows);
    }
  }
  return BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
      true, explicit_membership_rows);
}

bool CurrentTokenUserMatchesSid(PSID sid) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    CloseHandle(token);
    return false;
  }
  std::vector<unsigned char> buffer(size, 0);
  const bool loaded = GetTokenInformation(
      token, TokenUser, buffer.data(), size, &size) != 0;
  CloseHandle(token);
  if (!loaded) return false;
  const auto* token_user =
      reinterpret_cast<const TOKEN_USER*>(buffer.data());
  return EqualSid(token_user->User.Sid, sid) != FALSE;
}

bool GrantServiceIdentityAccess(const std::wstring& path,
                                PSID sid,
                                DWORD inheritance) {
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD read_status = GetNamedSecurityInfoW(
      const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr,
      &descriptor);
  if (read_status != ERROR_SUCCESS) return false;
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = inheritance;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
  PACL new_dacl = nullptr;
  const DWORD merge_status =
      SetEntriesInAclW(1, &access, old_dacl, &new_dacl);
  DWORD write_status = merge_status;
  if (merge_status == ERROR_SUCCESS) {
    write_status = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
  }
  if (new_dacl != nullptr) LocalFree(new_dacl);
  if (descriptor != nullptr) LocalFree(descriptor);
  return merge_status == ERROR_SUCCESS && write_status == ERROR_SUCCESS;
}

#else

std::vector<std::uint64_t> PlatformImplicitServiceGroupAllowlist() {
#ifdef __APPLE__
  return {kMacOsImplicitEveryoneGroupId,
          kMacOsImplicitLocalAccountsGroupId};
#else
  return {};
#endif
}

bool ServiceIdentityHasOnlyConfiguredGroup(const std::string& identity,
                                           gid_t primary_group,
                                           gid_t required_group) {
  if (primary_group != required_group) return false;
  int count = 0;
  (void)getgrouplist(identity.c_str(), primary_group, nullptr, &count);
  if (count <= 0) return false;
  std::vector<gid_t> groups(static_cast<std::size_t>(count));
  if (getgrouplist(identity.c_str(), primary_group, groups.data(), &count) < 0) {
    return false;
  }
  if (count <= 0 || static_cast<std::size_t>(count) > groups.size()) {
    return false;
  }
  std::vector<std::uint64_t> resolved_group_ids;
  resolved_group_ids.reserve(static_cast<std::size_t>(count) + 1);
  resolved_group_ids.push_back(static_cast<std::uint64_t>(primary_group));
  for (int index = 0; index < count; ++index) {
    resolved_group_ids.push_back(
        static_cast<std::uint64_t>(groups[static_cast<std::size_t>(index)]));
  }
  return BootstrapServiceIdentityGroupSetIsLeastAuthority(
      resolved_group_ids, static_cast<std::uint64_t>(required_group),
      PlatformImplicitServiceGroupAllowlist());
}

bool CurrentProcessHasOnlyConfiguredGroup(gid_t required_group) {
  if (getgid() != required_group || getegid() != required_group) return false;
  const int count = getgroups(0, nullptr);
  if (count < 0) return false;
  std::vector<gid_t> groups(static_cast<std::size_t>(count));
  if (count > 0 && getgroups(count, groups.data()) != count) return false;
  std::vector<std::uint64_t> resolved_group_ids;
  resolved_group_ids.reserve(groups.size() + 2);
  resolved_group_ids.push_back(static_cast<std::uint64_t>(getgid()));
  resolved_group_ids.push_back(static_cast<std::uint64_t>(getegid()));
  for (const gid_t group_id : groups) {
    resolved_group_ids.push_back(static_cast<std::uint64_t>(group_id));
  }
  return BootstrapServiceIdentityGroupSetIsLeastAuthority(
      resolved_group_ids, static_cast<std::uint64_t>(required_group),
      PlatformImplicitServiceGroupAllowlist());
}

#endif

}  // namespace

bool BootstrapServiceIdentityGroupSetIsLeastAuthority(
    const std::vector<std::uint64_t>& resolved_group_ids,
    std::uint64_t configured_group_id,
    const std::vector<std::uint64_t>& implicit_group_allowlist) {
  if (configured_group_id == 0 || resolved_group_ids.empty()) return false;
  bool configured_group_seen = false;
  for (const std::uint64_t group_id : resolved_group_ids) {
    if (group_id == configured_group_id) {
      configured_group_seen = true;
      continue;
    }
    if (std::find(implicit_group_allowlist.begin(),
                  implicit_group_allowlist.end(),
                  group_id) == implicit_group_allowlist.end()) {
      return false;
    }
  }
  return configured_group_seen;
}

bool BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
    bool inventory_complete,
    const std::vector<bool>& explicit_membership_rows) {
  return inventory_complete &&
         std::none_of(explicit_membership_rows.begin(),
                      explicit_membership_rows.end(),
                      [](bool member) { return member; });
}

bool BootstrapPlatformProfileContractValidForPlatform(
    const BootstrapPlatformProfile& profile,
    std::string_view expected_platform) {
  const std::string normalized_expected =
      LowerAscii(std::string(expected_platform));
  return profile.schema_id == kBootstrapPlatformProfileSchema &&
         LowerAscii(profile.platform) == normalized_expected &&
         PlatformProfileIdentityContractValid(profile);
}

BootstrapPlatformProfileLoadResult LoadBootstrapPlatformProfile(
    const std::string& path) {
  BootstrapPlatformProfileLoadResult result;
  if (path.empty()) {
    result.protected_audit_detail = "platform_profile_path_required";
    return result;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.protected_audit_detail = "platform_profile_open_failed";
    return result;
  }
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(input, line)) {
    const auto comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    line = Trim(std::move(line));
    if (line.empty()) continue;
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      result.protected_audit_detail = "platform_profile_line_invalid";
      return result;
    }
    const std::string key = LowerAscii(Trim(line.substr(0, equals)));
    const std::string value = Trim(line.substr(equals + 1));
    if (key.empty() || value.empty() || !values.emplace(key, value).second) {
      result.protected_audit_detail = "platform_profile_field_invalid";
      return result;
    }
  }
  result.profile.schema_id = values["schema_id"];
  result.profile.platform = LowerAscii(values["platform"]);
  result.profile.service_identity = values["service_identity"];
  result.profile.service_group = values["service_group"];
  if (values.size() != 4 ||
      !BootstrapPlatformProfileContractValidForPlatform(result.profile,
                                                        CompiledPlatform())) {
    result.protected_audit_detail = "platform_profile_contract_invalid";
    return result;
  }
  result.ok = true;
  return result;
}

BootstrapOsAuthorityResult VerifyAndAssumeBootstrapServiceIdentity(
    const BootstrapPlatformProfile& profile) {
  if (!BootstrapPlatformProfileContractValidForPlatform(profile,
                                                        CompiledPlatform())) {
    return Denied("platform_profile_contract_invalid");
  }

#ifdef _WIN32
  if (!CurrentTokenIsAdministrator()) {
    return Denied("os_administrator_required");
  }
  std::vector<unsigned char> service_sid;
  if (!LookupIntendedManagedServiceIdentity(profile.service_identity,
                                            &service_sid)) {
    return Denied("configured_local_service_identity_not_found_or_not_local");
  }
  if (!ManagedServiceSidHasNoExplicitLocalGroupMembership(
          service_sid.data())) {
    return Denied(
        "service_identity_explicit_local_group_membership_forbidden_or_"
        "inventory_failed");
  }
  if (CurrentTokenUserMatchesSid(service_sid.data())) {
    return Denied("service_identity_must_be_distinct_from_admin_caller");
  }
  BootstrapOsAuthorityResult result;
  result.ok = true;
  result.ownership_handoff_ready = true;
  result.service_identity = profile.service_identity;
  result.service_group = profile.service_group;
  return result;
#else
  if (geteuid() != 0) {
    return Denied("os_root_required");
  }
  errno = 0;
  const group* configured_group = getgrnam(profile.service_group.c_str());
  if (configured_group == nullptr) {
    return Denied("configured_service_group_not_found");
  }
  const gid_t service_group_id = configured_group->gr_gid;
  errno = 0;
  const passwd* service_user = getpwnam(profile.service_identity.c_str());
  if (service_user == nullptr) {
    return Denied("configured_service_identity_not_found");
  }
  const uid_t service_user_id = service_user->pw_uid;
  const gid_t service_primary_group_id = service_user->pw_gid;
  if (service_user_id == 0) {
    return Denied("configured_service_identity_must_not_be_root");
  }
  if (!ServiceIdentityHasOnlyConfiguredGroup(profile.service_identity,
                                             service_primary_group_id,
                                             service_group_id)) {
    return Denied("configured_service_identity_group_set_not_least_authority");
  }
  if (initgroups(profile.service_identity.c_str(), service_group_id) != 0 ||
      setgid(service_group_id) != 0 || setuid(service_user_id) != 0) {
    return Denied("service_identity_assumption_failed");
  }
  if (geteuid() != service_user_id ||
      !CurrentProcessHasOnlyConfiguredGroup(service_group_id)) {
    return Denied("service_identity_assumption_not_effective");
  }
  BootstrapOsAuthorityResult result;
  result.ok = true;
  result.service_identity_assumed = true;
  result.ownership_handoff_ready = true;
  result.service_identity = profile.service_identity;
  result.service_group = profile.service_group;
  return result;
#endif
}

BootstrapOsAuthorityResult EnsureBootstrapServiceIdentityAccess(
    const BootstrapPlatformProfile& profile,
    const std::string& database_path) {
  if (!BootstrapPlatformProfileContractValidForPlatform(profile,
                                                        CompiledPlatform()) ||
      database_path.empty()) {
    return Denied("ownership_handoff_input_invalid");
  }
#ifdef _WIN32
  std::vector<unsigned char> service_sid;
  if (!LookupIntendedManagedServiceIdentity(profile.service_identity,
                                            &service_sid)) {
    return Denied("configured_local_service_identity_not_found_or_not_local");
  }
  const std::wstring database = Utf8ToWide(database_path);
  if (database.empty()) return Denied("database_path_encoding_invalid");
  const std::size_t separator = database.find_last_of(L"/\\");
  const std::wstring parent = separator == std::wstring::npos
                                  ? std::wstring(L".")
                                  : database.substr(0, separator);
  if (!GrantServiceIdentityAccess(
          parent, service_sid.data(),
          SUB_CONTAINERS_AND_OBJECTS_INHERIT)) {
    return Denied("service_identity_parent_acl_handoff_failed");
  }
  for (const std::wstring& path :
       {database, database + L".sb.txn_publish",
        database + L".sb.txn_publish.tmp"}) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    if (!GrantServiceIdentityAccess(path, service_sid.data(), NO_INHERITANCE)) {
      return Denied("service_identity_file_acl_handoff_failed");
    }
  }
  BootstrapOsAuthorityResult result;
  result.ok = true;
  result.ownership_handoff_ready = true;
  result.service_identity = profile.service_identity;
  result.service_group = profile.service_group;
  return result;
#else
  BootstrapOsAuthorityResult result;
  result.ok = true;
  result.service_identity_assumed = true;
  result.ownership_handoff_ready = true;
  result.service_identity = profile.service_identity;
  result.service_group = profile.service_group;
  return result;
#endif
}

}  // namespace scratchbird::cli
