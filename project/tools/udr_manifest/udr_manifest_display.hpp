// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/udr/packages/builtin/sb_udr_builtin_packages.hpp"
#include "uuid.hpp"
#include <sstream>
namespace scratchbird::client::udr_manifest {
inline std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += ch; break;
    }
  }
  return escaped;
}

inline std::string BuiltinUdrPackageDeploymentManifestJson() {
  const auto rows = scratchbird::udr::builtin_packages::BuiltinUdrPackageDeploymentManifest();
  std::ostringstream out;
  out << "{\"manifest_kind\":\"builtin_trusted_cpp_udr_packages\","
      << "\"public_abi_status\":\"frozen_builtin_udr_sb_udr_v1\","
      << "\"abi_version\":\"sb_udr_v1\","
      << "\"package_count\":" << rows.size() << ","
      << "\"packages\":[";
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto& row = rows[index];
    if (index != 0) out << ',';
    out << "{\"package_uuid\":\"" << EscapeJson(scratchbird::core::uuid::UuidToString(row.package_uuid)) << "\","
        << "\"package_name\":\"" << EscapeJson(row.package_name) << "\","
        << "\"family_id\":\"" << EscapeJson(row.family_id) << "\","
        << "\"category\":\"" << EscapeJson(row.category) << "\","
        << "\"abi_version\":\"" << EscapeJson(row.abi_version) << "\","
        << "\"runtime_language\":\"" << EscapeJson(row.runtime_language) << "\","
        << "\"source_revision\":\"" << EscapeJson(row.source_revision) << "\","
        << "\"binary_hash\":\"" << EscapeJson(row.binary_hash) << "\","
        << "\"signature_policy\":\"" << EscapeJson(row.signature_policy) << "\","
        << "\"capability_role\":\"" << EscapeJson(row.capability_role) << "\","
        << "\"release_policy\":\"" << EscapeJson(row.release_policy) << "\","
        << "\"diagnostic_code\":\"" << EscapeJson(row.diagnostic_code) << "\","
        << "\"entrypoints_csv\":\"" << EscapeJson(row.entrypoints_csv) << "\","
        << "\"install_component\":\"" << EscapeJson(row.install_component) << "\","
        << "\"public_abi_status\":\"" << EscapeJson(row.public_abi_status) << "\"}";
  }
  out << "]}";
  return out.str();
}

}  // namespace scratchbird::client::udr_manifest
