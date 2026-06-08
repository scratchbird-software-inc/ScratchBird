// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "expression/donor_variable_compatibility.hpp"

#include <array>
#include <cctype>
#include <string>

namespace scratchbird::parser::sbsql {
namespace {

using Descriptor = DonorVariableCompatibilityDescriptor;

std::string UpperAscii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const auto ch : value) {
    out.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool EqualAsciiFold(std::string_view lhs, std::string_view rhs) {
  return UpperAscii(lhs) == UpperAscii(rhs);
}

constexpr std::array<Descriptor, 29> kDonorVariableCompatibility = {{
    {"SBSQL-DCA62654CB0F", "@@", "tsql_mysql_system_variable_root", "", "", "",
     "", "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "SB_DIAG_DONOR_VARIABLE_NAME_REQUIRED",
     false, true, true},
    {"SBSQL-DE3C8D86F7F4", "@@ROWCOUNT", "tsql_mysql_system_variable",
     "ctx_last_row_count", "sb.fn.diagnostic.row_count", "SBSQL-FD4E4EFCCC17",
     "ROW_COUNT", "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-EED4041BEB12", "@@SERVERNAME", "tsql_system_variable_refusal",
     "policy.refusal.system_variable.servername",
     "sb.scalar.refusal_system_variable_servername", "SBSQL-FD4E4EFCCC17",
     "CURRENT_SERVER_NAME", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-463DCD391130", "@@SPID", "tsql_system_variable",
     "ctx_current_session_uuid", "sb.session.session_id", "SBSQL-FD4E4EFCCC17",
     "CURRENT_SESSION_UUID", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3", "", false,
     true, false},
    {"SBSQL-5D9C952A3697", "@@TRANCOUNT", "tsql_system_variable_refusal",
     "policy.refusal.system_variable.trancount",
     "sb.scalar.refusal_system_variable_trancount", "SBSQL-FD4E4EFCCC17",
     "CURRENT_TRANSACTION_DEPTH", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-C9CD649263EC", "@@VERSION", "tsql_mysql_system_variable",
     "ctx_current_engine_version", "sb.scalar.current_engine_version",
     "SBSQL-FD4E4EFCCC17", "CURRENT_ENGINE_VERSION",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-2DB83873C621", "@@autocommit", "mysql_system_variable_refusal",
     "policy.refusal.system_variable.autocommit",
     "sb.scalar.refusal_system_variable_autocommit", "SBSQL-FD4E4EFCCC17",
     "CURRENT_AUTOCOMMIT_MODE", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-9637EA2DFC5A", "@@global.var", "mysql_system_variable_refusal",
     "policy.refusal.system_variable.global_var",
     "sb.scalar.refusal_system_variable_global_var", "SBSQL-FD4E4EFCCC17",
     "GLOBAL_SETTING", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-35078B674F78", "@@global.version", "mysql_system_variable",
     "ctx_current_engine_version", "sb.scalar.current_engine_version",
     "SBSQL-FD4E4EFCCC17", "CURRENT_ENGINE_VERSION",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-7697E4BCE46F", "@@hostname", "mysql_system_variable_refusal",
     "policy.refusal.system_variable.hostname",
     "sb.scalar.refusal_system_variable_hostname", "SBSQL-FD4E4EFCCC17",
     "CURRENT_HOST_NAME", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-5FBC168DF633", "@@session.autocommit",
     "mysql_system_variable_refusal",
     "policy.refusal.system_variable.session_autocommit",
     "sb.scalar.refusal_system_variable_session_autocommit",
     "SBSQL-FD4E4EFCCC17", "CURRENT_SESSION_AUTOCOMMIT_MODE",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
     false, true, true},
    {"SBSQL-F000B704E26B", "@@session.time_zone", "mysql_system_variable",
     "ctx_current_timezone", "sb.scalar.current_setting_timezone",
     "SBSQL-FD4E4EFCCC17", "CURRENT_TIMEZONE",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-7269DD8C7658", "@@session.var", "mysql_system_variable_refusal",
     "policy.refusal.system_variable.session_var",
     "sb.scalar.refusal_system_variable_session_var", "SBSQL-FD4E4EFCCC17",
     "SESSION_SETTING", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-4BA3FC03F99E", "@@session.version", "mysql_system_variable",
     "ctx_current_engine_version", "sb.scalar.current_engine_version",
     "SBSQL-FD4E4EFCCC17", "CURRENT_ENGINE_VERSION",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-57121B14D3D2", "@@time_zone", "mysql_system_variable",
     "ctx_current_timezone", "sb.scalar.current_setting_timezone",
     "SBSQL-FD4E4EFCCC17", "CURRENT_TIMEZONE",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-F06055E58BA0", "@@transaction_isolation",
     "mysql_system_variable", "ctx_current_transaction_isolation",
     "sb.scalar.current_isolation_level", "SBSQL-FD4E4EFCCC17",
     "CURRENT_TRANSACTION_ISOLATION", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3", "", false,
     true, false},
    {"SBSQL-4798C99894E7", "@@tx_isolation", "mysql_system_variable",
     "ctx_current_transaction_isolation", "sb.scalar.current_isolation_level",
     "SBSQL-FD4E4EFCCC17", "CURRENT_TRANSACTION_ISOLATION",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-11D5ED7A686F", "RDB$GET_CONTEXT('SYSTEM','CURRENT_USER')",
     "firebird_context_variable", "ctx_current_user_uuid",
     "sb.session.current_user", "SBSQL-FD4E4EFCCC17", "CURRENT_USER_UUID",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-594209C32142", "RDB$GET_CONTEXT('SYSTEM','ENGINE_VERSION')",
     "firebird_context_variable", "ctx_current_engine_version",
     "sb.scalar.current_engine_version", "SBSQL-FD4E4EFCCC17",
     "CURRENT_ENGINE_VERSION", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3", "", false,
     true, false},
    {"SBSQL-C25F2B374483", "RDB$GET_CONTEXT('SYSTEM','TRANSACTION_ID')",
     "firebird_context_variable", "ctx_current_local_transaction_id",
     "sb.session.transaction_id", "SBSQL-FD4E4EFCCC17",
     "CURRENT_LOCAL_TRANSACTION_ID", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3", "", false,
     true, false},
    {"SBSQL-93B790433D9D", "RDB$GET_CONTEXT('USER_SESSION','CLIENT_PID')",
     "firebird_context_variable_refusal",
     "policy.refusal.rdb_context.client_pid",
     "sb.scalar.refusal_rdb_get_context_client_pid", "SBSQL-FD4E4EFCCC17",
     "CURRENT_CLIENT_PID", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-B26CC22AE57C", "RDB$GET_CONTEXT('USER_SESSION',name)",
     "firebird_context_variable_refusal",
     "policy.refusal.rdb_context.user_session_name",
     "sb.scalar.refusal_rdb_get_context_user_session_name",
     "SBSQL-FD4E4EFCCC17", "CURRENT_USER_SESSION_CONTEXT",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
     false, true, true},
    {"SBSQL-82082E76658A", "SYS_CONTEXT('USERENV','CLIENT_INFO')",
     "oracle_context_variable_refusal",
     "policy.refusal.sys_context.client_info",
     "sb.scalar.refusal_sys_context_client_info", "SBSQL-FD4E4EFCCC17",
     "CURRENT_CLIENT_INFO", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"SBSQL-B0DCA7477008", "SYS_CONTEXT('USERENV','CURRENT_USER')",
     "oracle_context_variable", "ctx_current_user_uuid",
     "sb.session.current_user", "SBSQL-FD4E4EFCCC17", "CURRENT_USER_UUID",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-B8F2EF583846", "SYS_CONTEXT('USERENV','SESSIONID')",
     "oracle_context_variable", "ctx_current_session_uuid",
     "sb.session.session_id", "SBSQL-FD4E4EFCCC17",
     "CURRENT_SESSION_UUID", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3", "", false,
     true, false},
    {"SBSQL-20BB356E693A", "SYS_CONTEXT('USERENV','SESSION_USER')",
     "oracle_context_variable", "ctx_current_user_uuid",
     "sb.session.current_user", "SBSQL-FD4E4EFCCC17", "CURRENT_USER_UUID",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "", false, true, false},
    {"SBSQL-73AAF62A5CC3", "SYS_CONTEXT('USERENV',name)",
     "oracle_context_variable_refusal",
     "policy.refusal.sys_context.userenv_name",
     "sb.scalar.refusal_sys_context_userenv_name", "SBSQL-FD4E4EFCCC17",
     "CURRENT_USERENV_CONTEXT", "expression.system_variable_read",
     "SBLR_SYSTEM_VARIABLE_READ", "sblr.expression.runtime.v3",
     "SB_DIAG_FUNCTION_RUNTIME_REFUSAL", false, true, true},
    {"DONOR-VARIABLE-COLON-IDENTIFIER", ":identifier",
     "donor_host_or_bind_variable", "ctx_donor_bind_variable",
     "", "SBSQL-FD4E4EFCCC17", "DONOR_BIND_VARIABLE",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "SB_DIAG_DONOR_VARIABLE_BIND_REQUIRED",
     false, true, true},
    {"DONOR-VARIABLE-AT-IDENTIFIER", "@identifier",
     "donor_session_variable", "ctx_donor_session_variable",
     "", "SBSQL-FD4E4EFCCC17", "DONOR_SESSION_VARIABLE",
     "expression.system_variable_read", "SBLR_SYSTEM_VARIABLE_READ",
     "sblr.expression.runtime.v3", "SB_DIAG_DONOR_VARIABLE_BIND_REQUIRED",
     false, true, true},
}};

}  // namespace

std::span<const DonorVariableCompatibilityDescriptor>
BuiltinDonorVariableCompatibilityDescriptors() {
  return {kDonorVariableCompatibility.data(),
          kDonorVariableCompatibility.size()};
}

const DonorVariableCompatibilityDescriptor*
FindDonorVariableCompatibilityBySurfaceId(std::string_view surface_id) {
  for (const auto& descriptor : BuiltinDonorVariableCompatibilityDescriptors()) {
    if (descriptor.surface_id == surface_id) return &descriptor;
  }
  return nullptr;
}

const DonorVariableCompatibilityDescriptor*
FindDonorVariableCompatibilityBySpelling(std::string_view donor_spelling) {
  for (const auto& descriptor : BuiltinDonorVariableCompatibilityDescriptors()) {
    if (EqualAsciiFold(descriptor.donor_spelling, donor_spelling)) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool IsDonorVariableCompatibilitySurface(std::string_view surface_id) {
  return FindDonorVariableCompatibilityBySurfaceId(surface_id) != nullptr;
}

bool IsDonorVariableCompatibilitySpelling(std::string_view donor_spelling) {
  return FindDonorVariableCompatibilityBySpelling(donor_spelling) != nullptr;
}

DonorVariableSblrBinding LowerDonorVariableCompatibilityBySpelling(
    std::string_view donor_spelling) {
  DonorVariableSblrBinding binding;
  const auto* descriptor =
      FindDonorVariableCompatibilityBySpelling(donor_spelling);
  if (descriptor == nullptr) {
    return binding;
  }

  binding.surface_id = std::string(descriptor->surface_id);
  binding.donor_spelling = std::string(descriptor->donor_spelling);
  binding.sblr_operation_id = std::string(descriptor->sblr_operation_id);
  binding.sblr_opcode = std::string(descriptor->sblr_opcode);
  binding.canonical_variable_id =
      std::string(descriptor->canonical_variable_id);
  binding.diagnostic_id = std::string(descriptor->diagnostic_id);
  binding.exact_refusal = descriptor->exact_refusal;
  if (!descriptor->canonical_variable_id.empty()) {
    binding.operands.push_back(
        {"text", "variable_id", std::string(descriptor->canonical_variable_id)});
  }
  binding.operands.push_back(
      {"text", "canonical_surface_id",
       std::string(descriptor->canonical_surface_id)});
  binding.operands.push_back(
      {"text", "donor_source_spelling",
       std::string(descriptor->donor_spelling)});
  if (descriptor->exact_refusal) {
    binding.operands.push_back({"text", "exact_refusal", "true"});
    if (!descriptor->diagnostic_id.empty()) {
      binding.operands.push_back(
          {"text", "refusal_diagnostic_id",
           std::string(descriptor->diagnostic_id)});
    }
    if (!descriptor->canonical_function_id.empty()) {
      binding.operands.push_back(
          {"text", "refusal_function_id",
           std::string(descriptor->canonical_function_id)});
    }
  }
  return binding;
}

}  // namespace scratchbird::parser::sbsql
