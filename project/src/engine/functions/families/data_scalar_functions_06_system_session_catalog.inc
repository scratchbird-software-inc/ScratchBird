// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
      return RefuseFunctionInvalidInput(request, "bit_xor arguments must be int64");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", left ^ right)});
  }
	  if (IdIs(id, {"data.scalar.bit_not", "sb.fn.numeric.bit_not"})) {
	    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "bit_not expects one integer argument");
	    std::int64_t value = 0;
	    if (!ValueAsInt64Strict(request.arguments[0].value, &value)) return RefuseFunctionInvalidInput(request, "bit_not argument must be int64");
	    return MakeFunctionSuccess(request, {MakeInt64Value("int64", ~value)});
	  }

  if (IdIs(id, {"data.scalar.bit_shift_left", "sb.fn.numeric.bit_shift_left", "sb.scalar.bit_shift_left", "bit_shift_left"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit_shift_left expects value and shift count");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t value = 0;
    std::int64_t shift = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &value) || !ValueAsInt64Strict(request.arguments[1].value, &shift)) {
      return RefuseFunctionInvalidInput(request, "bit_shift_left arguments must be int64");
    }
    if (shift < 0 || shift > 63) return RefuseFunctionNumericDomain(request, "bit_shift_left shift count must be in range 0 to 63");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(static_cast<std::uint64_t>(value) << shift))});
  }
  if (IdIs(id, {"data.scalar.bit_shift_right", "sb.fn.numeric.bit_shift_right", "sb.scalar.bit_shift_right", "bit_shift_right"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "bit_shift_right expects value and shift count");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    std::int64_t value = 0;
    std::int64_t shift = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &value) || !ValueAsInt64Strict(request.arguments[1].value, &shift)) {
      return RefuseFunctionInvalidInput(request, "bit_shift_right arguments must be int64");
    }
    if (shift < 0 || shift > 63) return RefuseFunctionNumericDomain(request, "bit_shift_right shift count must be in range 0 to 63");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", value >> shift)});
  }

  if (IdIs(id, {"data.scalar.session_id", "sb.session.session_id", "session_id",
                "current_session_id", "current_session_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "session_id expects no arguments");
    if (request.context.sblr_context.session_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.session_uuid)});
  }

  if (IdIs(id, {"data.scalar.transaction_id", "sb.session.transaction_id", "transaction_id",
                "current_transaction_id", "txid_current"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction_id expects no arguments");
    if (!request.context.sblr_context.transaction_context_present || request.context.sblr_context.local_transaction_id == 0) {
      return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    }
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", request.context.sblr_context.local_transaction_id)});
  }

  if (IdIs(id, {"data.scalar.mga_snapshot_id", "sb.scalar.mga_snapshot_id", "mga_snapshot_id"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "mga_snapshot_id expects no arguments");
    if (!request.context.sblr_context.transaction_context_present ||
        request.context.sblr_context.local_transaction_id == 0) {
      return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    }
    return MakeFunctionSuccess(
        request,
        {MakeUint64Value("uint64", request.context.sblr_context.snapshot_visible_through_local_transaction_id)});
  }

  if (IdIs(id, {"data.scalar.txid_status", "sb.session.txid_status", "txid_status"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "txid_status expects zero or one transaction id argument");
    }
    if (request.arguments.empty()) {
      if (!request.context.sblr_context.transaction_context_present ||
          request.context.sblr_context.local_transaction_id == 0) {
        return MakeFunctionSuccess(request, {MakeNullValue("character")});
      }
      return MakeFunctionSuccess(request, {MakeTextValue("character", "in_progress")});
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t probe_transaction_id = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &probe_transaction_id)) {
      return RefuseFunctionInvalidInput(request, "txid_status argument must be int64");
    }
    if (probe_transaction_id > 0 &&
        request.context.sblr_context.transaction_context_present &&
        request.context.sblr_context.local_transaction_id != 0 &&
        static_cast<std::uint64_t>(probe_transaction_id) ==
            request.context.sblr_context.local_transaction_id) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", "in_progress")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", "unknown")});
  }

  if (IdIs(id, {"data.scalar.pg_xact_status", "sb.session.pg_xact_status", "pg_xact_status"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "pg_xact_status expects zero or one transaction id argument");
    }
    if (request.arguments.empty()) {
      if (!request.context.sblr_context.transaction_context_present ||
          request.context.sblr_context.local_transaction_id == 0) {
        return MakeFunctionSuccess(request, {MakeNullValue("character")});
      }
      return MakeFunctionSuccess(request, {MakeTextValue("character", "in_progress")});
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    std::int64_t probe_transaction_id = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &probe_transaction_id)) {
      return RefuseFunctionInvalidInput(request, "pg_xact_status argument must be int64");
    }
    if (probe_transaction_id > 0 &&
        request.context.sblr_context.transaction_context_present &&
        request.context.sblr_context.local_transaction_id != 0 &&
        static_cast<std::uint64_t>(probe_transaction_id) ==
            request.context.sblr_context.local_transaction_id) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", "in_progress")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", "unknown")});
  }

  if (IdIs(id, {"data.scalar.savepoint_active", "sb.session.savepoint_active", "savepoint_active"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "savepoint_active expects zero or one savepoint name argument");
    }
    if (!request.arguments.empty() && IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    bool active = false;
    if (request.context.sblr_context.transaction_context_present &&
        request.context.sblr_context.local_transaction_id != 0) {
      if (request.arguments.empty()) {
        active = !request.context.sblr_context.active_savepoint_names.empty();
      } else {
        const auto name = ValueAsText(request.arguments[0].value);
        active = std::find(request.context.sblr_context.active_savepoint_names.begin(),
                           request.context.sblr_context.active_savepoint_names.end(),
                           name) != request.context.sblr_context.active_savepoint_names.end();
      }
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", active ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.transaction_uuid", "sb.session.transaction_uuid", "transaction_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction_uuid expects no arguments");
    if (!request.context.sblr_context.transaction_context_present || request.context.sblr_context.transaction_uuid.empty()) {
      return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.transaction_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_statement_uuid", "sb.session.current_statement_uuid",
                "current_statement_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_statement_uuid expects no arguments");
    if (request.context.sblr_context.statement_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.statement_uuid)});
  }

  if (IdIs(id, {"data.scalar.server_version", "sb.scalar.server_version", "server_version"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "server_version expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "ScratchBird 0.1.0")});
  }

  if (IdIs(id, {"data.scalar.server_version_num", "sb.scalar.server_version_num", "server_version_num"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "server_version_num expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 100)});
  }

  if (IdIs(id, {"data.scalar.set_config",
                "sb.scalar.set_config",
                "sb.scalar.set_config_name_value_is_local",
                "set_config",
                "set_config(name,value,is_local)"})) {
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("character", "session_config.local_context.route")});
    }
    if (request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "set_config expects zero arguments or name, value, is_local");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        IsSqlNull(request.arguments[1].value) ||
        IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    bool is_local = false;
    if (!ParseBooleanText(request.arguments[2].value, &is_local)) {
      return RefuseFunctionInvalidInput(request, "set_config is_local must be boolean");
    }
    auto setting_name = NormalizeSessionConfigName(ValueAsText(request.arguments[0].value));
    if (setting_name.empty()) {
      return RefuseFunctionInvalidInput(request, "set_config setting name must not be empty");
    }
    auto setting_value = ValueAsText(request.arguments[1].value);
    SetSessionConfigValue(request.context.sblr_context,
                          std::move(setting_name),
                          setting_value,
                          is_local);
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(setting_value))});
  }

  if (IdIs(id, {"data.scalar.pid", "sb.scalar.pid", "pid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "pid expects no arguments");
    return MakeFunctionSuccess(
        request,
        {MakeUint64Value("uint64", EffectiveBackendProcessId(request.context.sblr_context))});
  }

  if (IdIs(id, {"data.scalar.current_setting_timezone", "sb.scalar.current_setting_timezone",
                "current_timezone"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_setting('timezone') expects no arguments after binding");
    return MakeFunctionSuccess(
        request,
        {MakeTextValue(
            "character",
            SessionConfigValue(request.context.sblr_context, "timezone").value_or("UTC"))});
  }

  if (IdIs(id, {"data.scalar.current_setting", "sb.scalar.current_setting", "current_setting",
                "current_setting(name[,missing_ok])"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "current_setting expects name and optional missing_ok");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    bool missing_ok = false;
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
      if (!ParseBooleanText(request.arguments[1].value, &missing_ok)) {
        return RefuseFunctionInvalidInput(request, "current_setting missing_ok must be boolean");
      }
    }
    const auto setting_name = NormalizeSessionConfigName(ValueAsText(request.arguments[0].value));
    if (const auto setting_value = SessionConfigValue(request.context.sblr_context, setting_name)) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", *setting_value)});
    }
    if (missing_ok) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return RefuseFunctionInvalidInput(request, "current_setting setting is unknown");
  }

  if (IdIs(id, {"data.scalar.temp_buffers", "sb.scalar.temp_buffers", "temp_buffers"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "temp_buffers expects no arguments");
    const auto configured =
        SessionConfigValue(request.context.sblr_context, "temp_buffers").value_or("8388608");
    std::uint64_t value = 0;
    if (!ValueAsUint64Strict(MakeTextValue("character", configured), &value)) {
      return RefuseFunctionInvalidInput(request, "temp_buffers session setting must be uint64 bytes");
    }
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", value)});
  }

  if (IdIs(id, {"data.scalar.pg_advisory_lock",
                "sb.scalar.pg_advisory_lock",
                "sb.scalar.pg_advisory_lock_key",
                "pg_advisory_lock",
                "pg_advisory_lock(key)"})) {
    if (request.arguments.empty()) {
      AppendAdvisoryLockEvidence(request.context.sblr_context,
                                 "pg_advisory_lock.marker:no_key");
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("character", "session_advisory_lock.local_context.route")});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_lock expects zero arguments or one advisory lock key");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t key = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &key)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_lock key must be int64");
    }
    bool acquired = false;
    if (!AcquireSessionAdvisoryLock(
            request.context.sblr_context, "pg_advisory_lock", key, &acquired)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_lock requires SBLR session runtime state");
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(acquired)});
  }

  if (IdIs(id, {"data.scalar.pg_try_advisory_lock",
                "sb.scalar.pg_try_advisory_lock",
                "sb.scalar.pg_try_advisory_lock_key",
                "pg_try_advisory_lock",
                "pg_try_advisory_lock(key)"})) {
    if (request.arguments.empty()) {
      AppendAdvisoryLockEvidence(request.context.sblr_context,
                                 "pg_try_advisory_lock.marker:no_key");
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_try_advisory_lock expects zero arguments or one advisory lock key");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t key = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &key)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_try_advisory_lock key must be int64");
    }
    bool acquired = false;
    if (!AcquireSessionAdvisoryLock(
            request.context.sblr_context, "pg_try_advisory_lock", key, &acquired)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_try_advisory_lock requires SBLR session runtime state");
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(acquired)});
  }

  if (IdIs(id, {"data.scalar.pg_advisory_unlock",
                "sb.scalar.pg_advisory_unlock",
                "sb.scalar.pg_advisory_unlock_key",
                "pg_advisory_unlock",
                "pg_advisory_unlock(key)"})) {
    if (request.arguments.empty()) {
      AppendAdvisoryLockEvidence(request.context.sblr_context,
                                 "pg_advisory_unlock.marker:no_key");
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_unlock expects zero arguments or one advisory lock key");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t key = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &key)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_unlock key must be int64");
    }
    bool released = false;
    if (!ReleaseSessionAdvisoryLock(
            request.context.sblr_context, "pg_advisory_unlock", key, &released)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_unlock requires SBLR session runtime state");
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(released)});
  }

  if (IdIs(id, {"data.scalar.get_lock",
                "sb.scalar.get_lock",
                "get_lock",
                "GET_LOCK",
                "get_lock(name,timeout)"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(
          request,
          "get_lock expects lock name and timeout arguments");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const auto lock_name = ValueAsText(request.arguments[0].value);
    if (lock_name.empty()) {
      return RefuseFunctionInvalidInput(request, "get_lock name must not be empty");
    }
    std::int64_t timeout = 0;
    if (!ValueAsInt64Strict(request.arguments[1].value, &timeout)) {
      return RefuseFunctionInvalidInput(request, "get_lock timeout must be int64 seconds");
    }
    const auto key = AdvisoryLockKeyFromText(lock_name);
    bool acquired = false;
    if (!AcquireSessionAdvisoryLock(
            request.context.sblr_context, "get_lock", key, &acquired)) {
      return RefuseFunctionInvalidInput(
          request,
          "get_lock requires SBLR session runtime state");
    }
    if (!acquired) {
      AppendAdvisoryLockEvidence(request.context.sblr_context,
                                 "get_lock.timeout:" + std::to_string(key) +
                                     ":seconds=" + std::to_string(timeout));
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", acquired ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.release_lock",
                "sb.scalar.release_lock",
                "release_lock",
                "RELEASE_LOCK",
                "release_lock(name)"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(
          request,
          "release_lock expects one lock name argument");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    const auto lock_name = ValueAsText(request.arguments[0].value);
    if (lock_name.empty()) {
      return RefuseFunctionInvalidInput(request, "release_lock name must not be empty");
    }
    const auto key = AdvisoryLockKeyFromText(lock_name);
    bool released = false;
    if (!ReleaseSessionAdvisoryLock(
            request.context.sblr_context, "release_lock", key, &released)) {
      return RefuseFunctionInvalidInput(
          request,
          "release_lock requires SBLR session runtime state");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", released ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.pg_advisory_xact_lock",
                "sb.scalar.pg_advisory_xact_lock",
                "sb.scalar.pg_advisory_xact_lock_key",
                "pg_advisory_xact_lock",
                "pg_advisory_xact_lock(key)"})) {
    if (request.arguments.empty()) {
      AppendTransactionAdvisoryLockEvidence(request.context.sblr_context,
                                            "pg_advisory_xact_lock.marker:no_key");
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("character", "transaction_advisory_lock.local_context.route")});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_xact_lock expects zero arguments or one advisory lock key");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t key = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &key)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_xact_lock key must be int64");
    }
    bool acquired = false;
    if (!AcquireTransactionAdvisoryLock(
            request.context.sblr_context, "pg_advisory_xact_lock", key, &acquired)) {
      return RefuseFunctionInvalidInput(
          request,
          "pg_advisory_xact_lock requires SBLR transaction context");
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(acquired)});
  }

  if (IdIs(id, {"data.scalar.pg_cancel_backend",
                "sb.scalar.pg_cancel_backend",
                "sb.scalar.pg_cancel_backend_pid",
                "pg_cancel_backend",
                "pg_cancel_backend(pid)"})) {
    if (request.arguments.empty()) {
      AppendBackendControlEvidence(request.context.sblr_context, "pg_cancel_backend.marker:no_target");
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "pg_cancel_backend expects zero or one pid argument");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t signed_pid = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &signed_pid) || signed_pid <= 0) {
      return RefuseFunctionInvalidInput(request, "pg_cancel_backend pid must be a positive int64");
    }
    const auto target_pid = static_cast<std::uint64_t>(signed_pid);
    const auto current_pid = EffectiveBackendProcessId(request.context.sblr_context);
    bool requested = target_pid == current_pid;
    if (!requested && request.context.sblr_context.session_runtime_state) {
      requested = BackendPidListed(
          request.context.sblr_context.session_runtime_state->cancellable_backend_pids,
          target_pid);
    }
    if (requested && request.context.sblr_context.session_runtime_state) {
      request.context.sblr_context.session_runtime_state->cancel_requested_backend_pids.push_back(
          target_pid);
    }
    AppendBackendControlEvidence(
        request.context.sblr_context,
        std::string("pg_cancel_backend.") + (requested ? "requested:" : "unknown:") +
            std::to_string(target_pid));
    return MakeFunctionSuccess(request, {MakeBooleanValue(requested)});
  }

  if (IdIs(id, {"data.scalar.pg_terminate_backend",
                "sb.scalar.pg_terminate_backend",
                "sb.scalar.pg_terminate_backend_pid",
                "pg_terminate_backend",
                "pg_terminate_backend(pid)"})) {
    if (request.arguments.empty()) {
      AppendBackendControlEvidence(request.context.sblr_context, "pg_terminate_backend.marker:no_target");
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "pg_terminate_backend expects zero or one pid argument");
    }
    if (IsSqlNull(request.arguments[0].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    std::int64_t signed_pid = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &signed_pid) || signed_pid <= 0) {
      return RefuseFunctionInvalidInput(request, "pg_terminate_backend pid must be a positive int64");
    }
    const auto target_pid = static_cast<std::uint64_t>(signed_pid);
    const auto current_pid = EffectiveBackendProcessId(request.context.sblr_context);
    bool requested = false;
    if (target_pid == current_pid) {
      AppendBackendControlEvidence(
          request.context.sblr_context,
          "pg_terminate_backend.self_blocked:" + std::to_string(target_pid));
      return MakeFunctionSuccess(request, {MakeBooleanValue(false)});
    }
    if (request.context.sblr_context.session_runtime_state) {
      requested = BackendPidListed(
          request.context.sblr_context.session_runtime_state->terminable_backend_pids,
          target_pid);
      if (requested) {
        request.context.sblr_context.session_runtime_state->terminate_requested_backend_pids.push_back(
            target_pid);
      }
    }
    AppendBackendControlEvidence(
        request.context.sblr_context,
        std::string("pg_terminate_backend.") + (requested ? "requested:" : "unknown:") +
            std::to_string(target_pid));
    return MakeFunctionSuccess(request, {MakeBooleanValue(requested)});
  }

  if (IdIs(id, {"data.scalar.default_charset", "sb.scalar.default_charset", "default_charset"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "default_charset expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "UTF-8")});
  }

  if (IdIs(id, {"data.scalar.default_collation", "sb.scalar.default_collation", "default_collation"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "default_collation expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "unicode_root")});
  }

  if (IdIs(id, {"data.scalar.collation_for", "sb.scalar.collation_for", "collation_for"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "collation_for expects exactly one text argument");
    }
    const auto& value = request.arguments[0].value;
    if (IsSqlNull(value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", value.collation_name.empty() ? "unicode_root" : value.collation_name)});
  }

  if (IdIs(id, {"data.scalar.pg_typeof", "sb.scalar.pg_typeof", "pg_typeof"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "pg_typeof expects zero or one argument");
    }
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", "unknown")});
    }
    const auto& value = request.arguments[0].value;
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", value.descriptor_id.empty() ? "unknown" : value.descriptor_id)});
  }

  if (IdIs(id, {"data.scalar.descriptor_of", "sb.scalar.descriptor_of", "descriptor_of"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "descriptor_of expects exactly one argument");
    }
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("json_document", DescriptorDocumentJson(request.arguments[0].value))});
  }

  if (IdIs(id, {"data.scalar.value_state", "sb.scalar.value_state", "value_state"})) {
    if (request.arguments.size() != 1) {
      return RefuseFunctionInvalidInput(request, "value_state expects exactly one argument");
    }
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", ValueStateName(request.arguments[0].value))});
  }

  if (IdIs(id, {"data.scalar.catalog_object_owner",
                "sb.scalar.catalog_object_owner",
                "catalog_object_owner"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "catalog_object_owner expects zero or one UUID argument");
    }
    std::optional<CatalogContextObject> object;
    if (request.arguments.empty()) {
      object = CatalogObjectForName(request.context.sblr_context, "current_database", "database");
    } else if (!IsSqlNull(request.arguments[0].value)) {
      object = CatalogObjectForUuid(request.context.sblr_context, ValueAsText(request.arguments[0].value));
    }
    if (!object || object->owner_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", object->owner_uuid)});
  }

  if (IdIs(id, {"data.scalar.catalog_object_uuid",
                "sb.scalar.catalog_object_uuid",
                "catalog_object_uuid"})) {
    if (request.arguments.size() > 2) {
      return RefuseFunctionInvalidInput(request, "catalog_object_uuid expects zero, one, or two arguments");
    }
    if (request.arguments.empty()) {
      if (request.context.sblr_context.database_uuid.empty()) {
        return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
      }
      return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.database_uuid)});
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    }
    const std::string class_filter =
        request.arguments.size() == 2 ? ValueAsText(request.arguments[1].value) : std::string{};
    const auto object = CatalogObjectForName(
        request.context.sblr_context,
        ValueAsText(request.arguments[0].value),
        class_filter);
    if (!object) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", object->uuid)});
  }

  if (IdIs(id, {"data.scalar.catalog_object_name",
                "sb.scalar.catalog_object_name",
                "catalog_object_name"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "catalog_object_name expects zero or one UUID argument");
    }
    std::optional<CatalogContextObject> object;
    if (request.arguments.empty()) {
      object = CatalogObjectForName(request.context.sblr_context, "current_database", "database");
    } else if (!IsSqlNull(request.arguments[0].value)) {
      object = CatalogObjectForUuid(request.context.sblr_context, ValueAsText(request.arguments[0].value));
    }
    if (!object) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", object->name)});
  }

  if (IdIs(id, {"data.scalar.catalog_object_class",
                "sb.scalar.catalog_object_class",
                "catalog_object_class"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "catalog_object_class expects zero or one UUID argument");
    }
    std::optional<CatalogContextObject> object;
    if (request.arguments.empty()) {
      object = CatalogObjectForName(request.context.sblr_context, "current_database", "database");
    } else if (!IsSqlNull(request.arguments[0].value)) {
      object = CatalogObjectForUuid(request.context.sblr_context, ValueAsText(request.arguments[0].value));
    }
    if (!object) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", object->object_class)});
  }

  if (IdIs(id, {"data.scalar.has_table_privilege",
                "sb.scalar.has_table_privilege",
                "has_table_privilege"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "has_table_privilege expects table, privilege or user, table, privilege");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    const bool has_user_arg = request.arguments.size() == 3;
    const std::size_t table_index = has_user_arg ? 1 : 0;
    const std::size_t privilege_index = table_index + 1;
    bool allowed = false;
    if (PrivilegePrincipalAllowed(request, 0, has_user_arg)) {
      const auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
      const auto table = ResolvePrivilegeTable(
          request.context.sblr_context,
          api_context,
          ValueAsText(request.arguments[table_index].value));
      if (table) {
        const auto privilege = NormalizePrivilegeText(
            ValueAsText(request.arguments[privilege_index].value), "table");
        allowed = PrivilegeObjectAllowsCurrentPrincipal(
            request.context.sblr_context, api_context, *table, privilege);
      }
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(allowed)});
  }

  if (IdIs(id, {"data.scalar.has_column_privilege",
                "sb.scalar.has_column_privilege",
                "has_column_privilege"})) {
    if (request.arguments.size() != 3 && request.arguments.size() != 4) {
      return RefuseFunctionInvalidInput(
          request, "has_column_privilege expects table, column, privilege or user, table, column, privilege");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    const bool has_user_arg = request.arguments.size() == 4;
    const std::size_t table_index = has_user_arg ? 1 : 0;
    const std::size_t column_index = table_index + 1;
    const std::size_t privilege_index = column_index + 1;
    bool allowed = false;
    if (PrivilegePrincipalAllowed(request, 0, has_user_arg)) {
      const auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
      const auto table = ResolvePrivilegeTable(
          request.context.sblr_context,
          api_context,
          ValueAsText(request.arguments[table_index].value));
      if (table && PrivilegeTableHasColumn(*table, ValueAsText(request.arguments[column_index].value))) {
        const auto privilege = NormalizePrivilegeText(
            ValueAsText(request.arguments[privilege_index].value), "column");
        allowed = PrivilegeObjectAllowsCurrentPrincipal(
            request.context.sblr_context, api_context, *table, privilege);
      }
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(allowed)});
  }

  if (IdIs(id, {"data.scalar.has_function_privilege",
                "sb.scalar.has_function_privilege",
                "has_function_privilege"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(
          request, "has_function_privilege expects function, privilege or user, function, privilege");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    const bool has_user_arg = request.arguments.size() == 3;
    const std::size_t function_index = has_user_arg ? 1 : 0;
    const std::size_t privilege_index = function_index + 1;
    bool allowed = false;
    if (PrivilegePrincipalAllowed(request, 0, has_user_arg)) {
      const auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
      const auto function = ResolvePrivilegeFunction(
          request.context.sblr_context,
          ValueAsText(request.arguments[function_index].value));
      if (function) {
        const auto privilege = NormalizePrivilegeText(
            ValueAsText(request.arguments[privilege_index].value), "function");
        allowed = PrivilegeObjectAllowsCurrentPrincipal(
            request.context.sblr_context, api_context, *function, privilege);
      }
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(allowed)});
  }

  if (IdIs(id, {"data.scalar.has_schema_privilege",
                "sb.scalar.has_schema_privilege",
                "has_schema_privilege"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(
          request, "has_schema_privilege expects schema, privilege or user, schema, privilege");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    }
    const bool has_user_arg = request.arguments.size() == 3;
    const std::size_t schema_index = has_user_arg ? 1 : 0;
    const std::size_t privilege_index = schema_index + 1;
    bool allowed = false;
    if (PrivilegePrincipalAllowed(request, 0, has_user_arg)) {
      const auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
      const auto schema = ResolvePrivilegeSchema(
          request.context.sblr_context,
          ValueAsText(request.arguments[schema_index].value));
      if (schema) {
        const auto privilege = NormalizePrivilegeText(
            ValueAsText(request.arguments[privilege_index].value), "schema");
        allowed = PrivilegeObjectAllowsCurrentPrincipal(
            request.context.sblr_context, api_context, *schema, privilege);
      }
    }
    return MakeFunctionSuccess(request, {MakeBooleanValue(allowed)});
  }

  if (IdIs(id, {"data.scalar.descriptor_snapshot_id",
                "sb.scalar.descriptor_snapshot_id",
                "descriptor_snapshot_id"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "descriptor_snapshot_id expects no arguments");
    if (request.context.sblr_context.security_snapshot_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.security_snapshot_uuid)});
  }

  if (IdIs(id, {"data.scalar.execution_type_descriptor",
                "sb.scalar.execution_type_descriptor",
                "execution_type_descriptor",
                "executiontypedescriptor",
                "ExecutionTypeDescriptor"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "ExecutionTypeDescriptor expects no arguments");
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("json_document", ExecutionTypeDescriptorJson(request.context.sblr_context))});
  }

  if (IdIs(id, {"data.scalar.column_descriptor",
                "sb.scalar.column_descriptor",
                "column_descriptor"})) {
    if (!request.arguments.empty() && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "column_descriptor expects zero arguments or table_uuid and column_name");
    }
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("json_document", ContextColumnDescriptorSummaryJson(request.context.sblr_context))});
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    }
    const auto descriptor = ContextColumnDescriptorJson(
        request.context.sblr_context,
        ValueAsText(request.arguments[0].value),
        ValueAsText(request.arguments[1].value));
    if (!descriptor) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", *descriptor)});
  }

  if (IdIs(id, {"data.scalar.index_descriptor",
                "sb.scalar.index_descriptor",
                "index_descriptor"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "index_descriptor expects zero or one index UUID argument");
    }
    if (request.arguments.empty()) {
      return MakeFunctionSuccess(request, {MakeTextValue("json_document", IndexDescriptorSummaryJson())});
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  }

  if (IdIs(id, {"data.scalar.relation_row_estimate",
                "sb.scalar.relation_row_estimate",
                "relation_row_estimate"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "relation_row_estimate expects zero or one table UUID argument");
    }
    auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
    if (api_context.database_path.empty()) {
      return RefuseFunctionInvalidInput(request, "relation_row_estimate requires engine database context");
    }
    if (!request.arguments.empty()) {
      if (IsSqlNull(request.arguments[0].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      }
      const auto statistics = scratchbird::engine::internal_api::EstimateMgaRelationStatistics(
          api_context, TrimAscii(ValueAsText(request.arguments[0].value)), false);
      if (!statistics.ok) {
        const std::string detail = statistics.diagnostic.detail.empty()
                                       ? statistics.diagnostic.message_key
                                       : statistics.diagnostic.detail;
        return RefuseFunctionInvalidInput(request, "relation_row_estimate failed: " + detail);
      }
      if (!statistics.statistics.relation_found) {
        return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      }
      return MakeFunctionSuccess(
          request,
          {MakeUint64Value("uint64", statistics.statistics.visible_row_estimate)});
    }
    const auto statistics = scratchbird::engine::internal_api::EstimateMgaCatalogStatistics(api_context, false);
    if (!statistics.ok) {
      const std::string detail = statistics.diagnostic.detail.empty()
                                     ? statistics.diagnostic.message_key
                                     : statistics.diagnostic.detail;
      return RefuseFunctionInvalidInput(request, "relation_row_estimate failed: " + detail);
    }
    return MakeFunctionSuccess(
        request,
        {MakeUint64Value("uint64", statistics.statistics.visible_row_estimate)});
  }

  if (IdIs(id, {"data.scalar.table_size",
                "sb.scalar.table_size",
                "table_size"})) {
    if (request.arguments.size() > 2) {
      return RefuseFunctionInvalidInput(request, "table_size expects zero, one, or two arguments");
    }
    auto api_context = EngineRequestContextFromSblr(request.context.sblr_context);
    if (api_context.database_path.empty()) {
      return RefuseFunctionInvalidInput(request, "table_size requires engine database context");
    }
    bool include_indexes = true;
    if (request.arguments.size() == 2) {
      if (IsSqlNull(request.arguments[1].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      }
      if (!ParseBooleanText(request.arguments[1].value, &include_indexes)) {
        return RefuseFunctionInvalidInput(request, "table_size include_indexes must be boolean");
      }
    }
    if (!request.arguments.empty()) {
      if (IsSqlNull(request.arguments[0].value)) {
        return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      }
      const auto statistics = scratchbird::engine::internal_api::EstimateMgaRelationStatistics(
          api_context, TrimAscii(ValueAsText(request.arguments[0].value)), include_indexes);
      if (!statistics.ok) {
        const std::string detail = statistics.diagnostic.detail.empty()
                                       ? statistics.diagnostic.message_key
                                       : statistics.diagnostic.detail;
        return RefuseFunctionInvalidInput(request, "table_size failed: " + detail);
      }
      if (!statistics.statistics.relation_found) {
        return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
      }
      return MakeFunctionSuccess(
          request,
          {MakeUint64Value("uint64", statistics.statistics.table_size_bytes)});
    }
    const auto statistics = scratchbird::engine::internal_api::EstimateMgaCatalogStatistics(api_context, include_indexes);
    if (!statistics.ok) {
      const std::string detail = statistics.diagnostic.detail.empty()
                                     ? statistics.diagnostic.message_key
                                     : statistics.diagnostic.detail;
      return RefuseFunctionInvalidInput(request, "table_size failed: " + detail);
    }
    return MakeFunctionSuccess(
        request,
        {MakeUint64Value("uint64", statistics.statistics.table_size_bytes)});
  }

  if (IdIs(id, {"data.scalar.diagnostic_field",
                "sb.scalar.diagnostic_field",
                "diagnostic_field"})) {
    if (request.arguments.size() > 1) {
      return RefuseFunctionInvalidInput(request, "diagnostic_field expects zero or one field name argument");
    }
    if (request.arguments.empty()) {
      if (request.context.sblr_context.current_diagnostic_id.empty()) {
        return MakeFunctionSuccess(request, {MakeNullValue("character")});
      }
      return MakeFunctionSuccess(
          request,
          {MakeTextValue("character", request.context.sblr_context.current_diagnostic_id)});
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    const auto field = DiagnosticFieldText(request.context.sblr_context, ValueAsText(request.arguments[0].value));
    if (!field) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", *field)});
  }

  if (IdIs(id, {"data.scalar.diagnostic_count",
                "sb.scalar.diagnostic_count",
                "diagnostic_count"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "diagnostic_count expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", DiagnosticCount(request.context.sblr_context))});
  }

  if (IdIs(id, {"data.scalar.gdscode", "sb.scalar.gdscode", "gdscode"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "gdscode expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", GdscodeForContext(request.context.sblr_context))});
  }

  if (IdIs(id, {"data.scalar.last_error_position",
                "sb.scalar.last_error_position",
                "last_error_position"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "last_error_position expects no arguments");
    return MakeFunctionSuccess(request, {MakeNullValue("int64")});
  }

  if (IdIs(id, {"data.scalar.error_class", "sb.scalar.error_class", "error_class"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "error_class expects no arguments");
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", ErrorClassForSqlstate(request.context.sblr_context.current_sqlstate))});
  }

  if (IdIs(id, {"data.scalar.comparison_collation_resolution",
                "sb.scalar.comparison_collation_resolution",
                "comparison_collation_resolution"})) {
    if (!request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, "comparison_collation_resolution expects no arguments");
    }
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", "descriptor_collation_explicit_collate_override")});
  }

  if (IdIs(id, {"data.scalar.keyword_case_rule", "sb.scalar.keyword_case_rule", "keyword_case_rule"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "keyword_case_rule expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "case_insensitive")});
  }

  if (IdIs(id, {"data.scalar.quoted_identifier_case_rule",
                "sb.scalar.quoted_identifier_case_rule",
                "quoted_identifier_case_rule"})) {
    if (!request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, "quoted_identifier_case_rule expects no arguments");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", "spelling_preserving_not_identity")});
  }

  if (IdIs(id, {"data.scalar.unquoted_identifier_case_rule",
                "sb.scalar.unquoted_identifier_case_rule",
                "unquoted_identifier_case_rule"})) {
    if (!request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, "unquoted_identifier_case_rule expects no arguments");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", "case_preserving_case_insensitive")});
  }

  if (IdIs(id, {"data.scalar.unicode_root", "sb.scalar.unicode_root", "unicode_root"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "unicode_root expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "unicode_root")});
  }

  if (IdIs(id, {"data.scalar.array_max_dimension", "sb.scalar.array_max_dimension", "array_max_dimension"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "array_max_dimension expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 6)});
  }

  if (IdIs(id, {"data.scalar.array_max_element_count", "sb.scalar.array_max_element_count", "array_max_element_count"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "array_max_element_count expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1048576)});
  }

  if (IdIs(id, {"data.scalar.case_when_max_branches", "sb.scalar.case_when_max_branches", "case_when_max_branches"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "case_when_max_branches expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.cte_max_count_per_statement", "sb.scalar.cte_max_count_per_statement", "cte_max_count_per_statement"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "cte_max_count_per_statement expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.nested_subquery_max_depth", "sb.scalar.nested_subquery_max_depth", "nested_subquery_max_depth"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "nested_subquery_max_depth expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 256)});
  }

  if (IdIs(id, {"data.scalar.recursive_cte_max_depth", "sb.scalar.recursive_cte_max_depth", "recursive_cte_max_depth"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "recursive_cte_max_depth expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.result_set_max_columns", "sb.scalar.result_set_max_columns", "result_set_max_columns"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "result_set_max_columns expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 4096)});
  }

  if (IdIs(id, {"data.scalar.union_max_arms", "sb.scalar.union_max_arms", "union_max_arms"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "union_max_arms expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.numeric_division_by_zero", "sb.scalar.numeric_division_by_zero", "numeric_division_by_zero"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "numeric_division_by_zero expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "error")});
  }

  if (IdIs(id, {"data.scalar.localized_label_max_length_bytes",
                "sb.scalar.localized_label_max_length_bytes",
                "localized_label_max_length_bytes"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "localized_label_max_length_bytes expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.default_schema_resolution", "sb.scalar.default_schema_resolution", "default_schema_resolution"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "default_schema_resolution expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "session_search_path_ambiguous_context_refusal")});
  }

  if (IdIs(id, {"data.scalar.result_set_max_rows_in_response",
                "sb.scalar.result_set_max_rows_in_response",
                "result_set_max_rows_in_response"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "result_set_max_rows_in_response expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.identifier_max_length_bytes",
                "sb.scalar.identifier_max_length_bytes",
                "identifier_max_length_bytes"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "identifier_max_length_bytes expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 255)});
  }

  if (IdIs(id, {"data.scalar.statement_timeout",
                "sb.scalar.statement_timeout",
                "statement_timeout"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_timeout expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.statement_timeout_default",
                "sb.scalar.statement_timeout_default",
                "statement_timeout_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_timeout_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.statement_timeout_ms", "sb.scalar.statement_timeout_ms", "statement_timeout_ms"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_timeout_ms expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.client_min_messages_default",
                "sb.scalar.client_min_messages_default",
                "client_min_messages_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_min_messages_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "NOTICE")});
  }

  if (IdIs(id, {"data.scalar.numeric_overflow_behavior",
                "sb.scalar.numeric_overflow_behavior",
                "numeric_overflow_behavior"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "numeric_overflow_behavior expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "error")});
  }

  if (IdIs(id, {"data.scalar.null_ordering_default_for_asc",
                "sb.scalar.null_ordering_default_for_asc",
                "null_ordering_default_for_asc"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "null_ordering_default_for_asc expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "NULLS LAST")});
  }

  if (IdIs(id, {"data.scalar.statement_max_length_bytes",
                "sb.scalar.statement_max_length_bytes",
                "statement_max_length_bytes"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_max_length_bytes expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 33554432)});
  }

  if (IdIs(id, {"data.scalar.null_concat_returns_null",
                "sb.scalar.null_concat_returns_null",
                "null_concat_returns_null"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "null_concat_returns_null expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 1)});
  }

  if (IdIs(id, {"data.scalar.recursion_max_depth", "sb.scalar.recursion_max_depth", "recursion_max_depth"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "recursion_max_depth expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.parameter_marker_max_count",
                "sb.scalar.parameter_marker_max_count",
                "parameter_marker_max_count"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "parameter_marker_max_count expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 262144)});
  }

  if (IdIs(id, {"data.scalar.delimited_identifier_max_length_bytes",
                "sb.scalar.delimited_identifier_max_length_bytes",
                "delimited_identifier_max_length_bytes"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "delimited_identifier_max_length_bytes expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 255)});
  }

  if (IdIs(id, {"data.scalar.case_resolution_for_quoted_identifiers",
                "sb.scalar.case_resolution_for_quoted_identifiers",
                "case_resolution_for_quoted_identifiers"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "case_resolution_for_quoted_identifiers expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "exact_spelling_match_not_identity")});
  }

  if (IdIs(id, {"data.scalar.temporal_default_precision",
                "sb.scalar.temporal_default_precision",
                "temporal_default_precision"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "temporal_default_precision expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 6)});
  }

  if (IdIs(id, {"data.scalar.string_truncation_behavior",
                "sb.scalar.string_truncation_behavior",
                "string_truncation_behavior"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "string_truncation_behavior expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "error")});
  }

  if (IdIs(id, {"data.scalar.timezone_resolution", "sb.scalar.timezone_resolution", "timezone_resolution"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "timezone_resolution expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "session_timezone_explicit_zone_conversion")});
  }

  if (IdIs(id, {"data.scalar.null_in_aggregate_skipped",
                "sb.scalar.null_in_aggregate_skipped",
                "null_in_aggregate_skipped"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "null_in_aggregate_skipped expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 1)});
  }

  if (IdIs(id, {"data.scalar.null_ordering_default_for_desc",
                "sb.scalar.null_ordering_default_for_desc",
                "null_ordering_default_for_desc"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "null_ordering_default_for_desc expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "NULLS FIRST")});
  }

  if (IdIs(id, {"data.scalar.interval_default_precision",
                "sb.scalar.interval_default_precision",
                "interval_default_precision"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "interval_default_precision expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "qualifier_driven")});
  }

  if (IdIs(id, {"data.scalar.name_resolution", "sb.scalar.name_resolution", "name_resolution"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "name_resolution expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "uuid_bound_at_bind_time")});
  }

  if (IdIs(id, {"data.scalar.recursive_schema_path_separator",
                "sb.scalar.recursive_schema_path_separator",
                "recursive_schema_path_separator"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "recursive_schema_path_separator expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", ".")});
  }

  if (IdIs(id, {"data.scalar.identifier_max_length_chars",
                "sb.scalar.identifier_max_length_chars",
                "identifier_max_length_chars"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "identifier_max_length_chars expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 63)});
  }

  if (IdIs(id, {"data.scalar.lock_timeout", "sb.scalar.lock_timeout", "lock_timeout"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "lock_timeout expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.lock_timeout_default",
                "sb.scalar.lock_timeout_default",
                "lock_timeout_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "lock_timeout_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.lock_timeout_ms", "sb.scalar.lock_timeout_ms", "lock_timeout_ms"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "lock_timeout_ms expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.idle_in_transaction_session_timeout",
                "sb.scalar.idle_in_transaction_session_timeout",
                "idle_in_transaction_session_timeout"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "idle_in_transaction_session_timeout expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.idle_in_transaction_timeout_default",
                "sb.scalar.idle_in_transaction_timeout_default",
                "idle_in_transaction_timeout_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "idle_in_transaction_timeout_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.idle_in_transaction_session_timeout_ms",
                "sb.scalar.idle_in_transaction_session_timeout_ms",
                "idle_in_transaction_session_timeout_ms"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "idle_in_transaction_session_timeout_ms expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.transaction_timeout",
                "sb.scalar.transaction_timeout",
                "transaction_timeout"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction_timeout expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.transaction_timeout_default",
                "sb.scalar.transaction_timeout_default",
                "transaction_timeout_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction_timeout_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.client_addr", "sb.scalar.client_addr", "client_addr"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_addr expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "session.client_address")});
  }

  if (IdIs(id, {"data.scalar.client_port", "sb.scalar.client_port", "client_port"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_port expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.default_decimal_division_scale",
                "sb.scalar.default_decimal_division_scale",
                "default_decimal_division_scale"})) {
    if (!request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, "default_decimal_division_scale expects no arguments");
    }
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 6)});
  }

  if (IdIs(id, {"data.scalar.temp_buffer_size_default",
                "sb.scalar.temp_buffer_size_default",
                "temp_buffer_size_default"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "temp_buffer_size_default expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 8388608)});
  }

  if (IdIs(id, {"data.scalar.null_in_unique_constraint",
                "sb.scalar.null_in_unique_constraint",
                "null_in_unique_constraint"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "null_in_unique_constraint expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "multiple_nulls_allowed")});
  }

  if (IdIs(id, {"data.scalar.qualified_name_max_segments",
                "sb.scalar.qualified_name_max_segments",
                "qualified_name_max_segments"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "qualified_name_max_segments expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 16)});
  }

  if (IdIs(id, {"data.scalar.empty_string_equals_null",
                "sb.scalar.empty_string_equals_null",
                "empty_string_equals_null"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "empty_string_equals_null expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 0)});
  }

  if (IdIs(id, {"data.scalar.count_distinct_includes_null",
                "sb.scalar.count_distinct_includes_null",
                "count_distinct_includes_null"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "count_distinct_includes_null expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 0)});
  }

  if (IdIs(id, {"data.scalar.operation_evidence_required",
                "sb.scalar.operation_evidence_required",
                "operation_evidence_required"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "operation_evidence_required expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 1)});
  }

  if (IdIs(id, {"data.scalar.decision_proof_required",
                "sb.scalar.decision_proof_required",
                "decision_proof_required"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "decision_proof_required expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 1)});
  }

  if (IdIs(id, {"data.scalar.current_capability_set",
                "sb.scalar.current_capability_set",
                "current_capability_set"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_capability_set expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "public_noncluster_alpha")});
  }

  if (IdIs(id, {"data.scalar.application_name",
                "sb.scalar.application_name",
                "application_name"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "application_name expects no arguments");
    return MakeFunctionSuccess(
        request,
        {MakeTextValue("character", request.context.sblr_context.application_name)});
  }

  if (IdIs(id, {"data.scalar.current_engine_version",
                "sb.scalar.current_engine_version",
                "current_engine_version"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_engine_version expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "ScratchBird 0.1.0")});
  }

  if (IdIs(id, {"data.scalar.current_locale", "sb.scalar.current_locale", "current_locale"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_locale expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "en-US")});
  }

  if (IdIs(id, {"data.scalar.client_protocol", "sb.scalar.client_protocol", "client_protocol"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_protocol expects no arguments");
    if (request.context.sblr_context.client_protocol_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.client_protocol_uuid)});
  }

  if (IdIs(id, {"data.scalar.private_profile_active",
                "sb.scalar.private_profile_active",
                "private_profile_active"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "private_profile_active expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 0)});
  }

  if (IdIs(id, {"data.scalar.built_in_function_shadow_rule",
                "sb.scalar.built_in_function_shadow_rule",
                "built_in_function_shadow_rule"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "built_in_function_shadow_rule expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "deny_builtin_shadowing")});
  }

  if (IdIs(id, {"data.scalar.current_isolation_level",
                "sb.scalar.current_isolation_level",
                "current_isolation_level"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_isolation_level expects no arguments");
    const auto& isolation = request.context.sblr_context.transaction_isolation_level;
    return MakeFunctionSuccess(request, {MakeTextValue("character", isolation.empty() ? "read_committed" : isolation)});
  }

  if (IdIs(id, {"data.scalar.mga_isolation_profile",
                "sb.scalar.mga_isolation_profile",
                "mga_isolation_profile"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "mga_isolation_profile expects no arguments");
    const auto isolation = LowerAscii(request.context.sblr_context.transaction_isolation_level);
    std::string profile = "read_committed_statement_snapshot";
    if (isolation == "read_committed" || isolation == "read committed" ||
        isolation == "read_committed_statement" ||
        isolation == "read_committed_statement_snapshot") {
      profile = "read_committed_statement_snapshot";
    } else if (isolation == "read_committed_transaction_refresh") {
      profile = "read_committed_transaction_refresh";
    } else if (isolation == "snapshot" || isolation == "snapshot_transaction") {
      profile = "snapshot_transaction";
    } else if (isolation == "repeatable_read" || isolation == "repeatable read" ||
               isolation == "repeatable_read_snapshot") {
      profile = "repeatable_read_snapshot";
    } else if (isolation == "serializable" || isolation == "serializable_snapshot") {
      profile = "serializable_snapshot";
    } else if (isolation == "read_only" || isolation == "read_only_snapshot") {
      profile = "read_only_snapshot";
    } else if (isolation == "cluster_consistent" || isolation == "cluster_consistent_snapshot") {
      profile = "cluster_consistent_snapshot";
    } else if (isolation == "maintenance" || isolation == "maintenance_snapshot") {
      profile = "maintenance_snapshot";
    } else if (!isolation.empty()) {
      profile = isolation;
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", profile)});
  }

  if (IdIs(id, {"data.scalar.tx_read_only", "sb.scalar.tx_read_only", "tx_read_only"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "tx_read_only expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", request.context.sblr_context.read_only_mode ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.read_only_session",
                "sb.scalar.read_only_session",
                "read_only_session"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "read_only_session expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", request.context.sblr_context.read_only_mode ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.request_key_required",
                "sb.scalar.request_key_required",
                "request_key_required"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "request_key_required expects no arguments");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", 1)});
  }

  if (IdIs(id, {"data.scalar.sbsql_v3", "sb.scalar.sbsql_v3", "sbsql_v3"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sbsql_v3 expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "sbsql.v3")});
  }

  if (IdIs(id, {"data.scalar.sqlstate", "sb.scalar.sqlstate", "sqlstate"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sqlstate expects no arguments");
    const auto& sqlstate = request.context.sblr_context.current_sqlstate;
    return MakeFunctionSuccess(request, {MakeTextValue("character", sqlstate.empty() ? "00000" : sqlstate)});
  }

  if (IdIs(id, {"data.scalar.sqlcode", "sb.scalar.sqlcode", "sqlcode"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sqlcode expects no arguments");
    const auto& sqlstate = request.context.sblr_context.current_sqlstate;
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", sqlstate.empty() || sqlstate == "00000" ? 0 : -1)});
  }

  if (IdIs(id, {"data.scalar.sqlerrm", "sb.scalar.sqlerrm", "sqlerrm"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sqlerrm expects no arguments");
    const auto& sqlstate = request.context.sblr_context.current_sqlstate;
    if (sqlstate.empty() || sqlstate == "00000") return MakeFunctionSuccess(request, {MakeTextValue("character", "OK")});
    const auto& diagnostic = request.context.sblr_context.current_diagnostic_id;
    return MakeFunctionSuccess(request, {MakeTextValue("character", diagnostic.empty() ? std::string("SQLSTATE ") + sqlstate : diagnostic)});
  }

  if (IdIs(id, {"data.scalar.not_found", "sb.scalar.not_found", "not_found", "NOT_FOUND"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "not_found expects no arguments");
    const auto sqlstate = request.context.sblr_context.current_sqlstate;
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", sqlstate == "02000" ? 1 : 0)});
  }

  if (IdIs(id, {"data.scalar.signal", "sb.scalar.signal", "signal", "SIGNAL"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "signal expects no arguments in scalar diagnostic form");
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                        "SB_DIAG_PROCEDURAL_SIGNAL",
                                        "SIGNAL emitted SBsql procedural diagnostic");
  }

  if (IdIs(id, {"data.scalar.raise", "sb.scalar.raise", "raise", "RAISE"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "raise expects no arguments in scalar diagnostic form");
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                        "SB_DIAG_PROCEDURAL_RAISE",
                                        "RAISE emitted SBsql procedural diagnostic");
  }

  if (IdIs(id, {"data.scalar.resignal", "sb.scalar.resignal", "resignal", "RESIGNAL"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "resignal expects no arguments in scalar diagnostic form");
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::execution_failed,
                                        "SB_DIAG_PROCEDURAL_RESIGNAL",
                                        "RESIGNAL emitted SBsql procedural diagnostic");
  }

  if (IdIs(id, {"data.scalar.deprecated_keyword",
                "sb.scalar.deprecated_keyword",
                "deprecated_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "deprecated_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.deprecated")});
  }

  if (IdIs(id, {"data.scalar.reference_contextual_keyword",
                "sb.scalar.reference_contextual_keyword",
                "reference_contextual_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "reference_contextual_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.reference_contextual")});
  }

  if (IdIs(id, {"data.scalar.reserved_native_keyword",
                "sb.scalar.reserved_native_keyword",
                "reserved_native_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "reserved_native_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.reserved_native")});
  }

  if (IdIs(id, {"data.scalar.contextual_native_keyword",
                "sb.scalar.contextual_native_keyword",
                "contextual_native_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "contextual_native_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.contextual_native")});
  }

  if (IdIs(id, {"data.scalar.reference_reserved_keyword",
                "sb.scalar.reference_reserved_keyword",
                "reference_reserved_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "reference_reserved_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.reference_reserved")});
  }

  if (IdIs(id, {"data.scalar.meta_command_keyword",
                "sb.scalar.meta_command_keyword",
                "meta_command_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "meta_command_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.meta_command")});
  }

  if (IdIs(id, {"data.scalar.private_only_keyword",
                "sb.scalar.private_only_keyword",
                "private_only_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "private_only_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.private_only")});
  }

  if (IdIs(id, {"data.scalar.refusal_only_keyword",
                "sb.scalar.refusal_only_keyword",
                "refusal_only_keyword"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "refusal_only_keyword expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.refusal_only")});
  }

  if (IdIs(id, {"data.scalar.statement_terminator",
                "sb.scalar.statement_terminator",
                "statement_terminator"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_terminator expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "lexeme.statement_terminator")});
  }

  if (IdIs(id, {"data.scalar.comment_line", "sb.scalar.comment_line", "comment_line"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "comment_line expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "lexeme.comment_line")});
  }

  if (IdIs(id, {"data.scalar.comment_block", "sb.scalar.comment_block", "comment_block"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "comment_block expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "lexeme.comment_block")});
  }

  if (IdIs(id, {"data.scalar.current_request_uuid",
                "sb.scalar.current_request_uuid",
                "current_request_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_request_uuid expects no arguments");
    if (request.context.sblr_context.statement_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.statement_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_dialect_version",
                "sb.scalar.current_dialect_version",
                "current_dialect_version"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_dialect_version expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "sbsql.v3")});
  }

  if (IdIs(id, {"data.scalar.cardinality_violation",
                "sb.scalar.cardinality_violation",
                "cardinality_violation"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "cardinality_violation expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "21000")});
  }

  if (IdIs(id, {"data.scalar.currency", "sb.scalar.currency", "currency"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "currency expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "datatype.currency")});
  }

  if (IdIs(id, {"data.scalar.client_min_messages", "sb.scalar.client_min_messages", "client_min_messages"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_min_messages expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "NOTICE")});
  }

  if (IdIs(id, {"data.scalar.merge_action", "sb.scalar.merge_action", "merge_action"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "merge_action expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "dml.merge_action")});
  }

  if (IdIs(id, {"data.scalar.colocation", "sb.scalar.colocation", "colocation"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "colocation expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "physical_layout.colocation")});
  }

  if (IdIs(id, {"data.scalar.identifier_bare", "sb.scalar.identifier_bare", "identifier_bare"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "identifier_bare expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "identifier_class.bare")});
  }

  if (IdIs(id, {"data.scalar.search_path", "sb.scalar.search_path", "search_path"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "search_path expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "users.public")});
  }

  if (IdIs(id, {"data.scalar.sbsql_psql", "sb.scalar.sbsql_psql", "sbsql_psql"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sbsql_psql expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "sbsql.psql")});
  }

  if (IdIs(id, {"data.scalar.sql_variant", "sb.scalar.sql_variant", "sql_variant"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sql_variant expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "datatype.sql_variant")});
  }

  if (IdIs(id, {"data.scalar.random_seed", "sb.scalar.random_seed", "random_seed"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "random_seed expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 0)});
  }

  if (IdIs(id, {"data.scalar.recursion_limit", "sb.scalar.recursion_limit", "recursion_limit"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "recursion_limit expects no arguments");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", 1024)});
  }

  if (IdIs(id, {"data.scalar.performance", "sb.scalar.performance", "performance"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "performance expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "management.performance")});
  }

  if (IdIs(id, {"data.scalar.parser_only", "sb.scalar.parser_only", "parser_only"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "parser_only expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "scope.parser_only")});
  }

  if (IdIs(id, {"data.scalar.deprecated", "sb.scalar.deprecated", "deprecated"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "deprecated expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "keyword_class.deprecated")});
  }

  if (IdIs(id, {"data.scalar.filesystem", "sb.scalar.filesystem", "filesystem"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "filesystem expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "scope.filesystem_public_refused")});
  }

  if (IdIs(id, {"data.scalar.refuse", "sb.scalar.refuse", "refuse"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "refuse expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.refuse")});
  }

  if (IdIs(id, {"data.scalar.metrics", "sb.scalar.metrics", "metrics"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "metrics expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "management.metrics")});
  }

  if (IdIs(id, {"data.scalar.catalog_read", "sb.scalar.catalog_read", "catalog_read"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "catalog_read expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "authority.catalog_read")});
  }

  if (IdIs(id, {"data.scalar.reference_log_compatibility",
                "sb.scalar.reference_log_compatibility",
                "reference_log_compatibility"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "reference_log_compatibility expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "compatibility.reference_log_non_authority")});
  }

  if (IdIs(id, {"data.scalar.fail_closed", "sb.scalar.fail_closed", "fail_closed"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "fail_closed expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.fail_closed")});
  }

  if (IdIs(id, {"data.scalar.requires_new_function",
                "sb.scalar.requires_new_function",
                "requires_new_function"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "requires_new_function expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "implementation.requires_new_function")});
  }

  if (IdIs(id, {"data.scalar.random_seed_control",
                "sb.scalar.random_seed_control",
                "random_seed_control"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "random_seed_control expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "policy.random_seed_control")});
  }

  if (IdIs(id, {"data.scalar.evidence", "sb.scalar.evidence", "evidence"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "evidence expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "evidence.required")});
  }

  if (IdIs(id, {"data.scalar.private_profile_read",
                "sb.scalar.private_profile_read",
                "private_profile_read"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "private_profile_read expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "authority.private_profile_read")});
  }

  if (IdIs(id, {"data.scalar.evidence_chain_uuid",
                "sb.scalar.evidence_chain_uuid",
                "evidence_chain_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "evidence_chain_uuid expects no arguments");
    if (request.context.sblr_context.statement_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.statement_uuid)});
  }

  if (IdIs(id, {"data.scalar.parameter_marker", "sb.scalar.parameter_marker", "parameter_marker"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "parameter_marker expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "token.parameter_marker")});
  }

  if (IdIs(id, {"data.scalar.security", "sb.scalar.security", "security"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "security expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "management.security")});
  }

  if (IdIs(id, {"data.scalar.localized_label", "sb.scalar.localized_label", "localized_label"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "localized_label expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "label.localized")});
  }

  if (IdIs(id, {"data.scalar.policy_blocked", "sb.scalar.policy_blocked", "policy_blocked"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "policy_blocked expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.policy_blocked")});
  }

  if (IdIs(id, {"data.scalar.notice", "sb.scalar.notice", "notice"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "notice expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "NOTICE")});
  }

  if (IdIs(id, {"data.scalar.dictionary_encoded", "sb.scalar.dictionary_encoded", "dictionary_encoded"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "dictionary_encoded expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "encoding.dictionary")});
  }

  if (IdIs(id, {"data.scalar.unresolved", "sb.scalar.unresolved", "unresolved"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "unresolved expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.unresolved")});
  }

  if (IdIs(id, {"data.scalar.public", "sb.scalar.public", "public"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "public expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "schema.public")});
  }

  if (IdIs(id, {"data.scalar.tablegroup", "sb.scalar.tablegroup", "tablegroup"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "tablegroup expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "physical_layout.tablegroup")});
  }

  if (IdIs(id, {"data.scalar.none", "sb.scalar.none", "none"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "none expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "value.none")});
  }

  if (IdIs(id, {"data.scalar.descriptor", "sb.scalar.descriptor", "descriptor"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "descriptor expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "metadata.descriptor")});
  }

  if (IdIs(id, {"data.scalar.engine", "sb.scalar.engine", "engine"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "engine expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "authority.engine")});
  }

  if (IdIs(id, {"data.scalar.catalog", "sb.scalar.catalog", "catalog"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "catalog expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "authority.catalog")});
  }

  if (IdIs(id, {"data.scalar.unsupported", "sb.scalar.unsupported", "unsupported"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "unsupported expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.unsupported")});
  }

  if (IdIs(id, {"data.scalar.mergetree", "sb.scalar.mergetree", "mergetree"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "mergetree expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "physical_layout.mergetree")});
  }

  if (IdIs(id, {"data.scalar.refused", "sb.scalar.refused", "refused"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "refused expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.refused")});
  }

  if (IdIs(id, {"data.scalar.innodb", "sb.scalar.innodb", "innodb"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "innodb expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "reference_storage.innodb")});
  }

  if (IdIs(id, {"data.scalar.hnsw", "sb.scalar.hnsw", "hnsw"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "hnsw expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "index_method.hnsw")});
  }

  if (IdIs(id, {"data.scalar.sessions", "sb.scalar.sessions", "sessions"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sessions expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "management.sessions")});
  }

  if (IdIs(id, {"data.scalar.asof", "sb.scalar.asof", "asof"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "asof expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "temporal.asof")});
  }

  if (IdIs(id, {"data.scalar.post_event", "sb.scalar.post_event", "post_event"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "post_event expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "event.post")});
  }

  if (IdIs(id, {"data.scalar.regional", "sb.scalar.regional", "regional"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "regional expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "locality.regional")});
  }

  if (IdIs(id, {"data.scalar.ivf_flat", "sb.scalar.ivf_flat", "ivf_flat"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "ivf_flat expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "index_method.ivf_flat")});
  }

  if (IdIs(id, {"data.scalar.tsquery", "sb.scalar.tsquery", "tsquery"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "tsquery expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "datatype.tsquery")});
  }

  if (IdIs(id, {"data.scalar.client_address", "sb.scalar.client_address", "client_address"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "client_address expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "session.client_address")});
  }

  if (IdIs(id, {"data.scalar.txn", "sb.scalar.txn", "txn"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "txn expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "transaction.alias")});
  }

  if (IdIs(id, {"data.scalar.hierarchyid", "sb.scalar.hierarchyid", "hierarchyid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "hierarchyid expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "datatype.hierarchyid")});
  }

  if (IdIs(id, {"data.scalar.locality", "sb.scalar.locality", "locality"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "locality expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "physical_layout.locality")});
  }

  if (IdIs(id, {"data.scalar.unknown", "sb.scalar.unknown", "unknown"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "unknown expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "value.unknown")});
  }

  if (IdIs(id, {"data.scalar.sortop", "sb.scalar.sortop", "sortop"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sortop expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "operator.sortop")});
  }

  if (IdIs(id, {"data.scalar.customer_id", "sb.scalar.customer_id", "customer_id"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "customer_id expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.customer_id")});
  }

  if (IdIs(id, {"data.scalar.customers", "sb.scalar.customers", "customers"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "customers expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.customers")});
  }

  if (IdIs(id, {"data.scalar.sep", "sb.scalar.sep", "sep"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "sep expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.sep")});
  }

  if (IdIs(id, {"data.scalar.part", "sb.scalar.part", "part"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "part expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.part")});
  }

  if (IdIs(id, {"data.scalar.value", "sb.scalar.value", "value"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "value expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.value")});
  }

  if (IdIs(id, {"data.scalar.expr", "sb.scalar.expr", "expr"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "expr expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.expr")});
  }

  if (IdIs(id, {"data.scalar.private_surface_refused", "sb.scalar.private_surface_refused", "private_surface_refused"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "private_surface_refused expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.private_surface_refused")});
  }

  if (IdIs(id, {"data.scalar.no_request", "sb.scalar.no_request", "no_request"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "no_request expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.no_request")});
  }

  if (IdIs(id, {"data.scalar.no_statement", "sb.scalar.no_statement", "no_statement"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "no_statement expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.no_statement")});
  }

  if (IdIs(id, {"data.scalar.unsupported_refused_by_design", "sb.scalar.unsupported_refused_by_design", "unsupported_refused_by_design"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "unsupported_refused_by_design expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.unsupported_refused_by_design")});
  }

  if (IdIs(id, {"data.scalar.operator_overload_unresolved", "sb.scalar.operator_overload_unresolved", "operator_overload_unresolved"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "operator_overload_unresolved expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.operator_overload_unresolved")});
  }

  if (IdIs(id, {"data.scalar.no_transaction", "sb.scalar.no_transaction", "no_transaction"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "no_transaction expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.no_transaction")});
  }

  if (IdIs(id, {"data.scalar.requires_function_authoring", "sb.scalar.requires_function_authoring", "requires_function_authoring"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "requires_function_authoring expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "implementation.requires_function_authoring")});
  }

  if (IdIs(id, {"data.scalar.event_trigger_authority_unavailable", "sb.scalar.event_trigger_authority_unavailable", "event_trigger_authority_unavailable"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "event_trigger_authority_unavailable expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "authority.event_trigger_unavailable")});
  }

  if (IdIs(id, {"data.scalar.capability_required", "sb.scalar.capability_required", "capability_required"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "capability_required expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.capability_required")});
  }

  if (IdIs(id, {"data.scalar.psql_case_not_found", "sb.scalar.psql_case_not_found", "psql_case_not_found"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "psql_case_not_found expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "psql.case_not_found")});
  }

  if (IdIs(id, {"data.scalar.syntax_parser_only", "sb.scalar.syntax_parser_only", "syntax_parser_only"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "syntax_parser_only expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "syntax.parser_only")});
  }

  if (IdIs(id, {"data.scalar.reference_only_rewrite", "sb.scalar.reference_only_rewrite", "reference_only_rewrite"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "reference_only_rewrite expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "reference.rewrite_only")});
  }

  if (IdIs(id, {"data.scalar.object_resolution_failed", "sb.scalar.object_resolution_failed", "object_resolution_failed"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "object_resolution_failed expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.object_resolution_failed")});
  }

  if (IdIs(id, {"data.scalar.error_diagnostic_uuid",
                "sb.scalar.error_diagnostic_uuid",
                "error_diagnostic_uuid"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "error_diagnostic_uuid expects no arguments");
    if (request.context.sblr_context.current_diagnostic_uuid.empty()) {
      return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.current_diagnostic_uuid)});
  }

  struct ContextualKeywordCatalogScalar {
    std::string_view leaf;
    std::string_view value;
  };
  constexpr ContextualKeywordCatalogScalar kContextualKeywordCatalogScalars[] = {
      {"alter", "keyword.contextual.alter"},
      {"as", "keyword.contextual.as"},
      {"begin", "keyword.contextual.begin"},
      {"create", "keyword.contextual.create"},
      {"cross", "keyword.contextual.cross"},
      {"distinct", "keyword.contextual.distinct"},
      {"drop", "keyword.contextual.drop"},
      {"else", "keyword.contextual.else"},
      {"end", "keyword.contextual.end"},
      {"events", "keyword.contextual.events"},
      {"exists", "keyword.contextual.exists"},
      {"full", "keyword.contextual.full"},
      {"index", "keyword.contextual.index"},
      {"inner", "keyword.contextual.inner"},
      {"is", "keyword.contextual.is"},
      {"lateral", "keyword.contextual.lateral"},
      {"natural", "keyword.contextual.natural"},
      {"on", "keyword.contextual.on"},
      {"outer", "keyword.contextual.outer"},
      {"sequence", "keyword.contextual.sequence"},
      {"show", "keyword.contextual.show"},
      {"similar", "keyword.contextual.similar"},
      {"then", "keyword.contextual.then"},
      {"upsert", "keyword.contextual.upsert"},
      {"using", "keyword.contextual.using"},
      {"view", "keyword.contextual.view"},
      {"wait", "keyword.contextual.wait"},
      {"when", "keyword.contextual.when"},
      {"with", "keyword.contextual.with"},
      {"stmt_null", "statement.null"},
  };
  for (const auto& scalar : kContextualKeywordCatalogScalars) {
    const std::string sb_id = std::string("sb.scalar.") + std::string(scalar.leaf);
    const std::string data_id = std::string("data.scalar.") + std::string(scalar.leaf);
    if (id == scalar.leaf || id == sb_id || id == data_id) {
      if (!request.arguments.empty()) {
        return RefuseFunctionInvalidInput(request, std::string(scalar.leaf) + " expects no arguments");
      }
      return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(scalar.value))});
    }
  }

  if (IdIs(id, {"data.scalar.transaction", "sb.scalar.transaction", "transaction"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.transaction")});
  }

  if (IdIs(id, {"data.scalar.context_ambiguous", "sb.scalar.context_ambiguous", "context_ambiguous"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "context_ambiguous expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "diagnostic.context_ambiguous")});
  }

  if (IdIs(id, {"data.scalar.policy_blocked_diagnostic", "sb.scalar.policy_blocked_diagnostic", "policy_blocked_diagnostic"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "policy_blocked_diagnostic expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "decision.policy_blocked")});
  }

  if (IdIs(id, {"data.scalar.diag_sqlstate", "sb.scalar.diag_sqlstate", "diag_sqlstate"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "diag_sqlstate expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", request.context.sblr_context.current_sqlstate)});
  }

  if (IdIs(id, {"data.scalar.canonical_function_idempotency_requirement", "sb.scalar.canonical_function_idempotency_requirement", "canonical_function_idempotency_requirement"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "canonical_function_idempotency_requirement expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "metadata.idempotency_requirement")});
  }

  if (IdIs(id, {"data.scalar.deprecation_warning", "sb.scalar.deprecation_warning", "deprecation_warning"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "deprecation_warning expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "warning.deprecation")});
  }

  if (IdIs(id, {"data.scalar.syntax_unsupported", "sb.scalar.syntax_unsupported", "syntax_unsupported"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "syntax_unsupported expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "syntax.unsupported")});
  }

  if (IdIs(id, {"data.scalar.dynamic_sql_untrusted", "sb.scalar.dynamic_sql_untrusted", "dynamic_sql_untrusted"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "dynamic_sql_untrusted expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "sblr.dynamic_sql.untrusted")});
  }

  if (IdIs(id, {"data.scalar.udr_admission_denied", "sb.scalar.udr_admission_denied", "udr_admission_denied"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "udr_admission_denied expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "sblr.udr.admission_denied")});
  }

  if (IdIs(id, {"data.scalar.statement", "sb.scalar.statement", "statement"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.statement")});
  }

  if (IdIs(id, {"data.scalar.table", "sb.scalar.table", "table"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "table expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.table")});
  }

  if (IdIs(id, {"data.scalar.a", "sb.scalar.a", "a"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "a expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.a")});
  }

  if (IdIs(id, {"data.scalar.x", "sb.scalar.x", "x"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "x expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.x")});
  }

  if (IdIs(id, {"data.scalar.t", "sb.scalar.t", "t"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "t expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.t")});
  }

  if (IdIs(id, {"data.scalar.name", "sb.scalar.name", "name"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "name expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.name")});
  }

  if (IdIs(id, {"data.scalar.customer", "sb.scalar.customer", "customer"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "customer expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.customer")});
  }

  if (IdIs(id, {"data.scalar.session", "sb.scalar.session", "session"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "session expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("character", "fixture.identifier.session")});
  }

  if (IdIs(id, {"data.scalar.current_user", "sb.session.current_user", "current_user"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_user expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.user_uuid)});
  }

  if (IdIs(id, {"data.scalar.session_user", "sb.session.session_user", "session_user",
                "data.scalar.system_user", "sb.session.system_user", "system_user",
                "data.scalar.user", "sb.session.user", "user"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "session user identity scalars expect no arguments");
    if (request.context.sblr_context.user_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.user_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_catalog", "sb.session.current_catalog", "current_catalog"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_catalog expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.database_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_schema", "sb.session.current_schema", "current_schema"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_schema expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.current_schema_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_database", "sb.session.current_database", "current_database"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.database_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_role", "sb.session.current_role", "current_role"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.current_role_uuid)});
  }

  if (IdIs(id, {"data.scalar.current_server", "sb.session.current_server", "current_server"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_server expects no arguments");
    if (request.context.sblr_context.node_uuid.empty()) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.node_uuid)});
  }

  if (id == "data.scalar.current_transaction") {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.transaction_uuid)});
  }

  if (IdIs(id, {"data.scalar.statement_timestamp", "sb.temporal.statement_timestamp", "statement_timestamp"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "statement_timestamp expects no arguments");
    if (request.context.sblr_context.statement_timestamp.empty()) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", request.context.sblr_context.statement_timestamp)});
  }

  if (IdIs(id, {"data.scalar.transaction_timestamp", "sb.temporal.transaction_timestamp", "transaction_timestamp"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "transaction_timestamp expects no arguments");
    if (!request.context.sblr_context.transaction_context_present ||
        request.context.sblr_context.transaction_timestamp.empty()) {
      return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", request.context.sblr_context.transaction_timestamp)});
  }

  if (IdIs(id, {"data.scalar.clock_timestamp", "sb.temporal.clock_timestamp", "clock_timestamp"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "clock_timestamp expects no arguments");
    if (request.context.sblr_context.current_timestamp.empty()) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", request.context.sblr_context.current_timestamp)});
  }

  if (IdIs(id, {"data.scalar.timeofday", "sb.temporal.timeofday", "timeofday"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "timeofday expects no arguments");
    if (request.context.sblr_context.current_timestamp.empty()) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", request.context.sblr_context.current_timestamp)});
  }

  if (IdIs(id, {"data.scalar.localtime", "sb.temporal.localtime", "localtime"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "localtime expects no arguments");
    if (request.context.sblr_context.current_timestamp.empty()) return MakeFunctionSuccess(request, {MakeNullValue("time")});
    return MakeFunctionSuccess(request, {MakeTextValue("time", TimePartFromTimestamp(request.context.sblr_context.current_timestamp))});
  }

  if (IdIs(id, {"data.scalar.localtimestamp", "sb.temporal.localtimestamp", "localtimestamp"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "localtimestamp expects no arguments");
    if (request.context.sblr_context.current_timestamp.empty()) return MakeFunctionSuccess(request, {MakeNullValue("timestamp")});
    std::string timestamp = request.context.sblr_context.current_timestamp;
    const auto separator = timestamp.find_first_of("T ");
    if (separator != std::string::npos) {
      for (std::size_t i = separator + 1; i < timestamp.size(); ++i) {
        if ((timestamp[i] == 'Z' || timestamp[i] == '+' || timestamp[i] == '-') && i >= separator + 9) {
          timestamp = timestamp.substr(0, i);
          break;
        }
      }
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp", std::move(timestamp))});
  }

  if (IdIs(id, {"data.scalar.now", "data.scalar.current_timestamp", "sb.fn.temporal.current_timestamp", "sb.temporal.now", "sb.temporal.current_timestamp", "sb.special.current_timestamp_keyword", "now", "current_timestamp"})) {
    if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "current_timestamp expects no arguments");
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", CurrentTimestampText(request))});
  }

  if (IdIs(id, {"data.scalar.current_date", "sb.fn.temporal.current_date", "sb.temporal.current_date", "current_date"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("date", DatePartFromTimestamp(CurrentTimestampText(request)))});
  }

  if (IdIs(id, {"data.scalar.current_time", "sb.fn.temporal.current_time", "sb.temporal.current_time", "current_time"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("time", TimePartFromTimestamp(CurrentTimestampText(request)))});
  }

  if (IdIs(id, {"data.scalar.row_count", "sb.fn.diagnostic.row_count", "row_count"})) {
    if (!request.context.sblr_context.last_row_count_present) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", request.context.sblr_context.last_row_count)});
  }

  if (IdIs(id, {"data.scalar.make_date", "sb.fn.temporal.make_date", "sb.temporal.make_date", "make_date"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "make_date expects year month day");
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("date")});
    }
    for (const auto& argument : request.arguments) {
      if (!argument.value.has_int64_value) return RefuseFunctionInvalidInput(request, "make_date arguments must be int64");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("date", FourDigit(request.arguments[0].value.int64_value) + "-" +
                                                               TwoDigit(request.arguments[1].value.int64_value) + "-" +
                                                               TwoDigit(request.arguments[2].value.int64_value))});
  }

  if (IdIs(id, {"data.scalar.make_time", "sb.fn.temporal.make_time", "sb.temporal.make_time", "make_time"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "make_time expects hour minute second");
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("time")});
    }
    for (const auto& argument : request.arguments) {
      if (!argument.value.has_int64_value) return RefuseFunctionInvalidInput(request, "make_time arguments must be int64");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("time", TwoDigit(request.arguments[0].value.int64_value) + ":" +
                                                               TwoDigit(request.arguments[1].value.int64_value) + ":" +
                                                               TwoDigit(request.arguments[2].value.int64_value))});
  }

  if (IdIs(id, {"data.scalar.make_timestamp", "sb.fn.temporal.make_timestamp", "sb.temporal.make_timestamp", "make_timestamp"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 6) {
      return RefuseFunctionInvalidInput(request, "make_timestamp expects date/time text or year month day hour minute second");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp")});
    }
    if (request.arguments.size() == 2) {
      return MakeFunctionSuccess(request, {MakeTextValue("timestamp", ValueAsText(request.arguments[0].value) + "T" +
                                                                    ValueAsText(request.arguments[1].value))});
    }
    for (const auto& argument : request.arguments) {
      if (!argument.value.has_int64_value) {
        return RefuseFunctionInvalidInput(request, "make_timestamp six-argument constructor requires int64 arguments");
      }
    }
    const std::int64_t year = request.arguments[0].value.int64_value;
    const std::int64_t month = request.arguments[1].value.int64_value;
    const std::int64_t day = request.arguments[2].value.int64_value;
    const std::int64_t hour = request.arguments[3].value.int64_value;
    const std::int64_t minute = request.arguments[4].value.int64_value;
    const std::int64_t second = request.arguments[5].value.int64_value;
    if (month < 1 || month > 12) return RefuseFunctionInvalidInput(request, "make_timestamp month must be in range 1 to 12");
    static constexpr unsigned kMakeTimestampDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned days_in_month = kMakeTimestampDaysInMonth[static_cast<std::size_t>(month - 1)];
    if (month == 2) {
      const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
      if (leap) days_in_month = 29;
    }
    if (day < 1 || day > static_cast<std::int64_t>(days_in_month)) {
      return RefuseFunctionInvalidInput(request, "make_timestamp day is out of range for month");
    }
    if (hour < 0 || hour > 23) return RefuseFunctionInvalidInput(request, "make_timestamp hour must be in range 0 to 23");
    if (minute < 0 || minute > 59) return RefuseFunctionInvalidInput(request, "make_timestamp minute must be in range 0 to 59");
    if (second < 0 || second > 59) return RefuseFunctionInvalidInput(request, "make_timestamp second must be in range 0 to 59");
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp", FourDigit(year) + "-" + TwoDigit(month) + "-" +
                                                                  TwoDigit(day) + "T" + TwoDigit(hour) + ":" +
                                                                  TwoDigit(minute) + ":" + TwoDigit(second))});
  }

  if (IdIs(id, {"data.scalar.make_timestamptz", "sb.fn.temporal.make_timestamptz", "sb.temporal.make_timestamptz", "make_timestamptz"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "make_timestamptz expects date and time text with optional timezone");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    }
    std::string assembled = ValueAsText(request.arguments[0].value) + "T" + ValueAsText(request.arguments[1].value);
    if (request.arguments.size() == 3) {
      assembled += ValueAsText(request.arguments[2].value);
    } else {
      assembled += "Z";
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", std::move(assembled))});
  }

  if (IdIs(id, {"data.scalar.date_trunc", "sb.fn.temporal.date_trunc", "sb.temporal.date_trunc", "date_trunc", "timestamp_trunc"})) {
    if (request.arguments.size() != 2 && request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "date_trunc expects field and timestamp with optional UTC timezone");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp")});
    }
    if (request.arguments.size() == 3) {
      const auto timezone = LowerAscii(TrimAscii(ValueAsText(request.arguments[2].value)));
      if (timezone != "utc" && timezone != "z" && timezone != "+00:00" &&
          timezone != "etc/utc" && timezone != "gmt") {
        return RefuseFunctionInvalidInput(request, "date_trunc timezone support is limited to UTC in this runtime slice");
      }
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp", TruncateTimestamp(ValueAsText(request.arguments[1].value),
                                                                                      ValueAsText(request.arguments[0].value)))});
  }

  if (IdIs(id, {"data.scalar.day_name", "sb.temporal.day_name", "day_name"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "day_name expects date and optional English locale");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments.size() == 2 && !IsSupportedEnglishLocale(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "day_name locale support is limited to English/C locale");
    }
    ParsedTemporal temporal;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal)) {
      return RefuseFunctionInvalidInput(request, "day_name requires ISO-like date text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", DayNameEnglish(DayOfWeekSundayZero(temporal)))});
  }

  if (IdIs(id, {"data.scalar.month_name", "sb.temporal.month_name", "month_name"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "month_name expects date and optional English locale");
    }
    if (IsSqlNull(request.arguments[0].value) ||
        (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    if (request.arguments.size() == 2 && !IsSupportedEnglishLocale(ValueAsText(request.arguments[1].value))) {
      return RefuseFunctionInvalidInput(request, "month_name locale support is limited to English/C locale");
    }
    ParsedTemporal temporal;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal)) {
      return RefuseFunctionInvalidInput(request, "month_name requires ISO-like date text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", MonthNameEnglish(temporal.month))});
  }

  if (IdIs(id, {"data.scalar.epoch", "sb.temporal.epoch", "epoch"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "epoch expects one temporal argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    ParsedTemporal temporal;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal)) {
      return RefuseFunctionInvalidInput(request, "epoch requires ISO-like timestamp text");
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", TemporalToEpochSeconds(temporal))});
  }

  if (IdIs(id, {"data.scalar.from_unixtime", "sb.temporal.from_unixtime", "from_unixtime"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "from_unixtime expects one int64 epoch seconds argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    std::int64_t epoch_seconds = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &epoch_seconds)) {
      return RefuseFunctionInvalidInput(request, "from_unixtime argument must be int64 epoch seconds");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", FormatTimestampTz(TemporalFromEpochSeconds(epoch_seconds)))});
  }

  if (IdIs(id, {"data.scalar.months_between", "sb.temporal.months_between", "months_between"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "months_between expects two date arguments");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    ParsedTemporal left;
    ParsedTemporal right;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &left) ||
        !ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &right)) {
      return RefuseFunctionInvalidInput(request, "months_between requires ISO-like date text arguments");
    }
    const double month_delta =
        static_cast<double>((left.year - right.year) * 12 +
                            static_cast<std::int64_t>(left.month) -
                            static_cast<std::int64_t>(right.month));
    const bool both_last_day = left.day == DaysInMonth(left.year, left.month) &&
                               right.day == DaysInMonth(right.year, right.month);
    const double fractional = (left.day == right.day || both_last_day)
                                  ? 0.0
                                  : (static_cast<double>(left.day) - static_cast<double>(right.day)) / 31.0;
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", month_delta + fractional)});
  }

  if (IdIs(id, {"data.scalar.next_day", "sb.temporal.next_day", "next_day"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "next_day expects date and weekday");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("date")});
    }
    ParsedTemporal temporal;
    int target_dow = 0;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal) ||
        !ParseWeekdayText(ValueAsText(request.arguments[1].value), &target_dow)) {
      return RefuseFunctionInvalidInput(request, "next_day requires ISO-like date text and English weekday");
    }
    const int current_dow = DayOfWeekSundayZero(temporal);
    int delta = (target_dow - current_dow + 7) % 7;
    if (delta == 0) delta = 7;
    const ParsedTemporal out = TemporalFromEpochSeconds(TemporalToEpochSeconds(temporal) +
                                                        static_cast<std::int64_t>(delta) * 86400);
    return MakeFunctionSuccess(request, {MakeTextValue("date", FormatDate(out))});
  }

  if (IdIs(id, {"data.scalar.to_char", "sb.scalar.to_char", "to_char"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "to_char expects value and format");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("character")});
    }
    const std::string format = ValueAsText(request.arguments[1].value);
    ParsedTemporal temporal;
    if (ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal)) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", FormatTemporalPattern(temporal, format))});
    }
    const std::string formatted = FormatNumericPattern(request.arguments[0].value, format);
    if (formatted.empty()) return RefuseFunctionInvalidInput(request, "to_char value must be ISO-like temporal text or numeric");
    return MakeFunctionSuccess(request, {MakeTextValue("character", formatted)});
  }

  if (IdIs(id, {"data.scalar.to_date", "sb.scalar.to_date", "to_date"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "to_date expects text and format");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("date")});
    }
    ParsedTemporal temporal;
    if (!ParseDateTimeByFormat(ValueAsText(request.arguments[0].value),
                               ValueAsText(request.arguments[1].value),
                               &temporal)) {
      return RefuseFunctionInvalidInput(request, "to_date supports YYYY/MM/DD-style numeric masks");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("date", FormatDate(temporal))});
  }

  if (IdIs(id, {"data.scalar.to_timestamp", "sb.scalar.to_timestamp", "to_timestamp"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "to_timestamp expects text and format");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("timestamp")});
    }
    ParsedTemporal temporal;
    if (!ParseDateTimeByFormat(ValueAsText(request.arguments[0].value),
                               ValueAsText(request.arguments[1].value),
                               &temporal)) {
      return RefuseFunctionInvalidInput(request, "to_timestamp supports YYYY/MM/DD HH24:MI:SS-style numeric masks");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp", FormatTimestamp(temporal))});
  }

  if (IdIs(id, {"data.scalar.to_number", "sb.scalar.to_number", "to_number"}) && request.arguments.size() == 2) {
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    }
    double parsed = 0.0;
    if (!ParseNumberByFormat(ValueAsText(request.arguments[0].value),
                             ValueAsText(request.arguments[1].value),
                             &parsed)) {
      return RefuseFunctionInvalidInput(request, "to_number text is not compatible with numeric format");
    }
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", parsed)});
  }

  if (IdIs(id, {"data.scalar.make_interval", "sb.fn.temporal.make_interval", "sb.temporal.make_interval", "make_interval"})) {
    if (request.arguments.size() > 7) {
      return RefuseFunctionInvalidInput(request, "make_interval expects up to seven int64 arguments");
    }
    ParsedInterval interval;
    std::int64_t values[7] = {0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      if (IsSqlNull(request.arguments[i].value)) return MakeFunctionSuccess(request, {MakeNullValue("interval")});
      if (!ValueAsInt64Strict(request.arguments[i].value, &values[i])) {
        return RefuseFunctionInvalidInput(request, "make_interval arguments must be int64");
      }
    }
    interval.years = values[0];
    interval.months = values[1];
    interval.days = values[2] * 7 + values[3];
    interval.hours = values[4];
    interval.minutes = values[5];
    interval.seconds = values[6];
    if (IntervalHasMixedSigns(interval)) {
      return RefuseFunctionInvalidInput(request, "make_interval mixed-sign components are ambiguous");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("interval", FormatIntervalText(interval))});
  }

  if (IdIs(id, {"data.scalar.age", "sb.fn.temporal.age", "sb.temporal.age", "age"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "age expects timestamp and optional comparison timestamp");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("interval")});
    }
    ParsedTemporal start;
    ParsedTemporal finish;
    if (request.arguments.size() == 1) {
      if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &start)) {
        return RefuseFunctionInvalidInput(request, "age requires ISO-like timestamp text");
      }
      if (!ParseIsoLikeTemporal(CurrentTimestampText(request), &finish)) {
        return RefuseFunctionInvalidInput(request, "age comparison timestamp must be ISO-like timestamp text");
      }
    } else if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &finish) ||
               !ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &start)) {
      return RefuseFunctionInvalidInput(request, "age requires ISO-like timestamp text");
    }
    ParsedInterval interval;
    interval.seconds = TemporalToEpochSeconds(finish) - TemporalToEpochSeconds(start);
    return MakeFunctionSuccess(request, {MakeTextValue("interval", FormatIntervalText(interval))});
  }

  if (IdIs(id, {"data.scalar.age_in_days", "sb.fn.temporal.age_in_days", "sb.temporal.age_in_days", "age_in_days",
                "data.scalar.age_in_months", "sb.fn.temporal.age_in_months", "sb.temporal.age_in_months", "age_in_months",
                "data.scalar.age_in_years", "sb.fn.temporal.age_in_years", "sb.temporal.age_in_years", "age_in_years"})) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "age_in_* expects timestamp and optional comparison timestamp");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    ParsedTemporal start;
    ParsedTemporal finish;
    if (request.arguments.size() == 1) {
      if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &start)) {
        return RefuseFunctionInvalidInput(request, "age_in_* requires ISO-like timestamp text");
      }
      if (!ParseIsoLikeTemporal(CurrentTimestampText(request), &finish)) {
        return RefuseFunctionInvalidInput(request, "age_in_* comparison timestamp must be ISO-like timestamp text");
      }
    } else if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &finish) ||
               !ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &start)) {
      return RefuseFunctionInvalidInput(request, "age_in_* requires ISO-like timestamp text");
    }
    const std::int64_t days = (TemporalToEpochSeconds(finish) - TemporalToEpochSeconds(start)) / 86400;
    if (IdIs(id, {"data.scalar.age_in_months", "sb.fn.temporal.age_in_months", "sb.temporal.age_in_months", "age_in_months"})) {
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", WholeMonthsBetween(start, finish))});
    }
    if (IdIs(id, {"data.scalar.age_in_years", "sb.fn.temporal.age_in_years", "sb.temporal.age_in_years", "age_in_years"})) {
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", WholeMonthsBetween(start, finish) / 12)});
    }
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", days)});
  }

  if (IdIs(id, {"data.scalar.date_add", "sb.fn.temporal.date_add", "sb.temporal.date_add", "date_add",
                "data.scalar.date_sub", "sb.fn.temporal.date_sub", "sb.temporal.date_sub", "date_sub"})) {
    if (request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "date_add/date_sub expects temporal value and interval");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id.empty()
                                                            ? "timestamp"
                                                            : request.arguments[0].value.descriptor_id)});
    }
    ParsedTemporal temporal;
    ParsedInterval interval;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[0].value), &temporal) ||
        !ParseIsoIntervalText(ValueAsText(request.arguments[1].value), &interval)) {
      return RefuseFunctionInvalidInput(request, "date_add/date_sub requires ISO-like temporal text and interval text");
    }
    if (IdIs(id, {"data.scalar.date_sub", "sb.fn.temporal.date_sub", "sb.temporal.date_sub", "date_sub"})) {
      interval.years = -interval.years;
      interval.months = -interval.months;
      interval.days = -interval.days;
      interval.hours = -interval.hours;
      interval.minutes = -interval.minutes;
      interval.seconds = -interval.seconds;
    }
    const ParsedTemporal out = AddIntervalToTemporal(temporal, interval);
    const std::string descriptor = !out.has_time ? "date" : "timestamp";
    return MakeFunctionSuccess(request, {MakeTextValue(descriptor, descriptor == "date" ? FormatDate(out) : FormatTimestamp(out))});
  }

  if (IdIs(id, {"data.scalar.date_diff", "sb.fn.temporal.date_diff", "sb.temporal.date_diff", "date_diff"})) {
    if (request.arguments.size() != 3) {
      return RefuseFunctionInvalidInput(request, "date_diff expects part, start, and end");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    }
    ParsedTemporal start;
    ParsedTemporal finish;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &start) ||
        !ParseIsoLikeTemporal(ValueAsText(request.arguments[2].value), &finish)) {
      return RefuseFunctionInvalidInput(request, "date_diff requires ISO-like start and end temporal text");
    }
    const std::string part = LowerAscii(ValueAsText(request.arguments[0].value));
    const std::int64_t seconds = TemporalToEpochSeconds(finish) - TemporalToEpochSeconds(start);
    if (part == "second" || part == "seconds") return MakeFunctionSuccess(request, {MakeInt64Value("int64", seconds)});
    if (part == "minute" || part == "minutes") return MakeFunctionSuccess(request, {MakeInt64Value("int64", seconds / 60)});
    if (part == "hour" || part == "hours") return MakeFunctionSuccess(request, {MakeInt64Value("int64", seconds / 3600)});
    if (part == "day" || part == "days") return MakeFunctionSuccess(request, {MakeInt64Value("int64", seconds / 86400)});
    if (part == "month" || part == "months") {
      return MakeFunctionSuccess(request, {MakeInt64Value("int64",
          (finish.year - start.year) * 12 + static_cast<std::int64_t>(finish.month) -
          static_cast<std::int64_t>(start.month))});
    }
    if (part == "year" || part == "years") {
      return MakeFunctionSuccess(request, {MakeInt64Value("int64", finish.year - start.year)});
    }
    return RefuseFunctionInvalidInput(request, "date_diff part must be second, minute, hour, day, month, or year");
  }

  if (IdIs(id, {"data.scalar.date_bin", "sb.fn.temporal.date_bin", "sb.temporal.date_bin", "date_bin"})) {
    if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "date_bin expects stride, source, and origin");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value) ||
        IsSqlNull(request.arguments[2].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("timestamp")});
    }
    ParsedInterval stride;
    ParsedTemporal source;
    ParsedTemporal origin;
    std::int64_t stride_seconds = 0;
    if (!ParseIsoIntervalText(ValueAsText(request.arguments[0].value), &stride) ||
        !IntervalToSeconds(stride, &stride_seconds) || stride_seconds <= 0 ||
        !ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &source) ||
        !ParseIsoLikeTemporal(ValueAsText(request.arguments[2].value), &origin)) {
      return RefuseFunctionInvalidInput(request, "date_bin requires positive day/time stride and ISO-like source/origin");
    }
    const std::int64_t source_seconds = TemporalToEpochSeconds(source);
    const std::int64_t origin_seconds = TemporalToEpochSeconds(origin);
    std::int64_t delta = source_seconds - origin_seconds;
    std::int64_t buckets = delta / stride_seconds;
    if (delta < 0 && delta % stride_seconds != 0) --buckets;
    const ParsedTemporal out = TemporalFromEpochSeconds(origin_seconds + buckets * stride_seconds);
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp", FormatTimestamp(out))});
  }

  if (IdIs(id, {"data.scalar.timezone", "sb.fn.temporal.timezone", "sb.temporal.timezone", "timezone"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "timezone expects zone and timestamp");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
    }
    std::int64_t offset_seconds = 0;
    if (!ParseTimezoneOffsetSeconds(ValueAsText(request.arguments[0].value), &offset_seconds)) {
      return RefuseFunctionInvalidInput(request, "timezone expects UTC, GMT, or numeric offset zone");
    }
    ParsedTemporal temporal;
    if (!ParseIsoLikeTemporal(ValueAsText(request.arguments[1].value), &temporal)) {
      return RefuseFunctionInvalidInput(request, "timezone timestamp must be ISO-like temporal text");
    }
    const ParsedTemporal utc = TemporalFromEpochSeconds(TemporalToEpochSeconds(temporal) - offset_seconds);
    return MakeFunctionSuccess(request, {MakeTextValue("timestamp_tz", FormatTimestamp(utc) + "Z")});
  }

  if (IdIs(id, {"data.scalar.time_bucket", "sb.fn.temporal.time_bucket", "time_bucket"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "time_bucket expects bucket_seconds and epoch milliseconds");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("timestamp_epoch_ms")});
    if (!request.arguments[0].value.has_int64_value || !request.arguments[1].value.has_int64_value ||
        request.arguments[0].value.int64_value <= 0) {
      return RefuseFunctionInvalidInput(request, "time_bucket requires positive bucket seconds and int64 epoch milliseconds");
    }
    const std::int64_t bucket_ms = request.arguments[0].value.int64_value * 1000;
    const std::int64_t epoch_ms = request.arguments[1].value.int64_value;
    return MakeFunctionSuccess(request, {MakeInt64Value("timestamp_epoch_ms", (epoch_ms / bucket_ms) * bucket_ms)});
  }

  if (IdIs(id, {"data.scalar.interval_seconds", "sb.fn.temporal.interval_seconds", "make_interval_seconds"})) {
    if (request.arguments.size() != 1 || IsSqlNull(request.arguments[0].value) || !request.arguments[0].value.has_int64_value) {
      return RefuseFunctionInvalidInput(request, "interval_seconds expects one int64 seconds argument");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("interval", std::to_string(request.arguments[0].value.int64_value))});
  }

  if (IdIs(id, {"data.scalar.hex_encode", "sb.fn.encoding.hex_encode", "hex"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "hex_encode expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", HexEncodeBytes(RawBytesFromValue(request.arguments[0].value)))});
  }

  if (IdIs(id, {"sb.scalar.to_hex", "to_hex"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "to_hex expects one integer argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    std::int64_t v = 0;
    if (!ValueAsInt64Strict(request.arguments[0].value, &v)) {
      return RefuseFunctionInvalidInput(request, "to_hex requires integer input");
    }
    std::uint64_t u = static_cast<std::uint64_t>(v);
    if (u == 0) {
      return MakeFunctionSuccess(request, {MakeTextValue("character", std::string("0"))});
    }
    static constexpr char kHex[] = "0123456789abcdef";
    char buf[17];
    char* p = buf + sizeof(buf);
    *--p = '\0';
    while (u != 0) {
      *--p = kHex[u & 0xfu];
      u >>= 4;
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(p))});
  }

  if (IdIs(id, {"data.scalar.hex_decode", "data.scalar.from_hex", "sb.fn.encoding.hex_decode", "unhex", "from_hex"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "hex_decode expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    std::vector<std::uint8_t> bytes;
    if (!HexDecodeBytes(ValueAsText(request.arguments[0].value), &bytes)) return RefuseFunctionInvalidInput(request, "hex_decode input is not valid even-length hexadecimal text");
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
  }

  if (IdIs(id, {"data.scalar.base64_encode", "sb.fn.encoding.base64_encode", "to_base64", "base64_encode"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "base64_encode expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", Base64EncodeBytes(RawBytesFromValue(request.arguments[0].value)))});
  }

  if (IdIs(id, {"data.scalar.base64_decode", "sb.fn.encoding.base64_decode", "from_base64", "base64_decode"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "base64_decode expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("binary")});
    std::vector<std::uint8_t> bytes;
    if (!Base64DecodeBytes(ValueAsText(request.arguments[0].value), &bytes)) return RefuseFunctionInvalidInput(request, "base64_decode input is not valid base64 text");
    return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
  }

  if (IdIs(id, {"data.scalar.uuid_to_text", "sb.fn.encoding.uuid_to_text", "uuid_to_text"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "uuid_to_text expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", ValueAsText(request.arguments[0].value))});
  }

  if (IdIs(id, {"data.scalar.text_to_uuid", "sb.fn.encoding.text_to_uuid", "text_to_uuid"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "text_to_uuid expects one argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uuid")});
    const auto text = ValueAsText(request.arguments[0].value);
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
      return RefuseFunctionInvalidInput(request, "text_to_uuid expects canonical UUID text");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", text)});
  }

  if (IdIs(id, {"data.scalar.array", "data.scalar.list", "sb.fn.collection.array", "array_construct", "json_array"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("array", JsonArrayFromArguments(request.arguments))});
  }

  if (IdIs(id, {"data.scalar.cardinality", "sb.fn.collection.cardinality", "sb.scalar.cardinality", "cardinality"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "cardinality expects one collection argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
    const auto count = SplitCollectionText(ValueAsText(request.arguments[0].value)).size();
    return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(count))});
  }

  if (IdIs(id, {"data.scalar.array_length", "array_length"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "array_length expects one collection argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", SplitCollectionText(ValueAsText(request.arguments[0].value)).size())});
  }

  if (IdIs(id, {"data.scalar.array_get", "sb.fn.collection.array_get", "array_get"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "array_get expects collection and zero-based index");
