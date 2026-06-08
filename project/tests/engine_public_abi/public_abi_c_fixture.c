// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"

#include <stdint.h>
#include <string.h>

static sb_engine_uuid_t test_uuid(uint8_t tail) {
  sb_engine_uuid_t uuid;
  memset(&uuid, 0, sizeof(uuid));
  uuid.bytes[0] = 0x01;
  uuid.bytes[6] = 0x70;
  uuid.bytes[15] = tail;
  return uuid;
}

int main(void) {
  if (sb_engine_abi_version_packed() != SB_ENGINE_ABI_VERSION_PACKED) {
    return 1;
  }

  const char* build_id = 0;
  uint64_t build_id_size = 0;
  if (sb_engine_abi_build_id(&build_id, &build_id_size) != SB_ENGINE_STATUS_OK) {
    return 2;
  }
  if (build_id == 0 || build_id_size == 0) {
    return 3;
  }

  sb_engine_open_params_v1_t open_params;
  memset(&open_params, 0, sizeof(open_params));
  open_params.struct_size = sizeof(open_params);
  open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;

  sb_engine_handle_t engine = 0;
  sb_engine_result_t result = 0;
  if (sb_engine_open(&open_params, &engine, &result) != SB_ENGINE_STATUS_OK) {
    if (result != 0) {
      (void)sb_engine_result_release(result);
    }
    return 4;
  }

  sb_engine_session_params_v1_t session_params;
  memset(&session_params, 0, sizeof(session_params));
  session_params.struct_size = sizeof(session_params);
  session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  session_params.effective_user_uuid = test_uuid(1);
  session_params.session_uuid = test_uuid(2);
  session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  session_params.default_language_utf8 = "en";
  session_params.default_language_size = 2;

  sb_engine_session_t session = 0;
  if (sb_engine_session_begin(engine, &session_params, &session, &result) != SB_ENGINE_STATUS_OK) {
    (void)sb_engine_close(engine, 0);
    return 5;
  }

  sb_engine_request_context_v1_t context;
  memset(&context, 0, sizeof(context));
  context.struct_size = sizeof(context);
  context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  context.effective_user_uuid = session_params.effective_user_uuid;
  context.session_uuid = session_params.session_uuid;
  context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
  context.rights_set_ref = 1;
  context.capability_set_ref = 1;

  sb_engine_sblr_dispatch_params_v1_t dispatch;
  memset(&dispatch, 0, sizeof(dispatch));
  dispatch.struct_size = sizeof(dispatch);
  dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;

  if (sb_engine_dispatch_sblr(session, 0, &context, &dispatch, &result) != SB_ENGINE_STATUS_OK) {
    (void)sb_engine_session_end(session, 0, 0);
    (void)sb_engine_close(engine, 0);
    return 6;
  }
  if (result == 0) {
    (void)sb_engine_session_end(session, 0, 0);
    (void)sb_engine_close(engine, 0);
    return 7;
  }

  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK) {
    return 8;
  }
  if (result_class != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
    return 9;
  }
  if (sb_engine_result_release(result) != SB_ENGINE_STATUS_OK) {
    return 10;
  }

  sb_engine_session_end_params_v1_t end_params;
  memset(&end_params, 0, sizeof(end_params));
  end_params.struct_size = sizeof(end_params);
  end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end_params.rollback_active_transactions = 1;
  end_params.cancel_open_results = 1;
  if (sb_engine_session_end(session, &end_params, 0) != SB_ENGINE_STATUS_OK) {
    return 11;
  }
  if (sb_engine_close(engine, 0) != SB_ENGINE_STATUS_OK) {
    return 12;
  }
  return 0;
}
