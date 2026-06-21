// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_ENGINE_H
#define SCRATCHBIRD_ENGINE_ENGINE_H

#include "scratchbird/engine/diagnostic.h"
#include "scratchbird/engine/export.h"
#include "scratchbird/engine/version.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sb_engine_handle_s* sb_engine_handle_t;
typedef struct sb_engine_session_s* sb_engine_session_t;
typedef struct sb_engine_transaction_s* sb_engine_transaction_t;
typedef struct sb_engine_result_s* sb_engine_result_t;

typedef enum sb_engine_open_mode_t {
  SB_ENGINE_OPEN_NORMAL = 0,
  SB_ENGINE_OPEN_READ_ONLY = 1,
  SB_ENGINE_OPEN_MAINTENANCE = 2,
  SB_ENGINE_OPEN_VALIDATION_ONLY = 3
} sb_engine_open_mode_t;

typedef enum sb_engine_trust_mode_t {
  SB_ENGINE_TRUST_SERVER_ISOLATED = 0,
  SB_ENGINE_TRUST_EMBEDDED_TRUSTED = 1,
  SB_ENGINE_TRUST_PARSER_UNTRUSTED = 2
} sb_engine_trust_mode_t;

typedef enum sb_engine_result_class_t {
  SB_ENGINE_RESULT_NONE = 0,
  SB_ENGINE_RESULT_ROW_BATCH = 1,
  SB_ENGINE_RESULT_COMMAND_COMPLETION = 2,
  SB_ENGINE_RESULT_EXECUTION_SUMMARY = 3,
  SB_ENGINE_RESULT_DIAGNOSTIC_ONLY = 4,
  SB_ENGINE_RESULT_CAPABILITY_REPORT = 5,
  SB_ENGINE_RESULT_METRIC_ROOT = 6
} sb_engine_result_class_t;

typedef struct sb_engine_uuid_t {
  uint8_t bytes[16];
} sb_engine_uuid_t;

typedef struct sb_engine_budget_v1_t {
  uint64_t deadline_unix_ms;
  uint64_t monotonic_timeout_ms;
  uint64_t cpu_units;
  uint64_t memory_bytes;
  uint64_t temporary_bytes;
  uint64_t output_rows;
  uint64_t output_bytes;
  uint64_t compile_budget_units;
  uint32_t recursion_depth;
  uint32_t opcode_count;
} sb_engine_budget_v1_t;

typedef struct sb_engine_open_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* database_path_utf8;
  uint64_t database_path_size;
  sb_engine_open_mode_t mode;
  uint8_t reserved_mode;
  uint8_t reserved_bytes[7];
  sb_engine_budget_v1_t resource_defaults;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_open_params_v1_t;

typedef struct sb_engine_session_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  sb_engine_uuid_t effective_user_uuid;
  sb_engine_uuid_t session_uuid;
  const char* default_language_utf8;
  uint64_t default_language_size;
  sb_engine_trust_mode_t trust_mode;
  uint32_t flags;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_session_params_v1_t;

typedef struct sb_engine_session_end_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint8_t rollback_active_transactions;
  uint8_t cancel_open_results;
  uint8_t reserved_bytes[6];
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_session_end_params_v1_t;

typedef struct sb_engine_transaction_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t isolation_level;
  uint32_t access_mode;
  uint32_t autocommit_mode;
  uint32_t flags;
  uint64_t timeout_ms;
  uint64_t idle_timeout_ms;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_transaction_params_v1_t;

typedef struct sb_engine_transaction_finish_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t timeout_ms;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_transaction_finish_params_v1_t;

typedef struct sb_engine_request_context_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  sb_engine_uuid_t effective_user_uuid;
  sb_engine_uuid_t session_uuid;
  sb_engine_uuid_t parser_package_uuid;
  sb_engine_uuid_t dialect_profile_uuid;
  sb_engine_trust_mode_t trust_mode;
  uint32_t flags;
  uint64_t rights_set_ref;
  uint64_t capability_set_ref;
  uint64_t source_artifact_set_ref;
  uint64_t transaction_ref;
  sb_engine_budget_v1_t resource_budget;
  uint8_t idempotency_key[32];
  uint64_t idempotency_key_size;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_request_context_v1_t;

typedef struct sb_engine_sblr_dispatch_params_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  const uint8_t* envelope_bytes;
  uint64_t envelope_size_bytes;
  const uint8_t* data_packet_bytes;
  uint64_t data_packet_size_bytes;
  uint32_t result_contract;
  uint32_t flags;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_sblr_dispatch_params_v1_t;

typedef struct sb_engine_command_completion_view_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  sb_engine_string_view_t operation_id;
  uint64_t affected_rows;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_command_completion_view_v1_t;

typedef struct sb_engine_execution_summary_view_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t elapsed_us;
  uint64_t rows_produced;
  uint64_t diagnostics_count;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_execution_summary_view_v1_t;

typedef struct sb_engine_row_batch_view_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t row_count;
  uint8_t end_of_stream;
  uint8_t reserved_bytes[7];
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_row_batch_view_v1_t;

typedef struct sb_engine_batch_request_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t max_rows;
  uint64_t max_bytes;
  uint64_t timeout_ms;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_batch_request_v1_t;

typedef struct sb_engine_capability_request_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t flags;
  uint32_t reserved_flags;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_capability_request_v1_t;

typedef struct sb_engine_metric_request_v1_t {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* root_path_utf8;
  uint64_t root_path_size;
  uint32_t flags;
  uint32_t reserved_flags;
  uint64_t reserved0;
  uint64_t reserved1;
} sb_engine_metric_request_v1_t;

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_open(const sb_engine_open_params_v1_t* params,
               sb_engine_handle_t* out_engine,
               sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_close(sb_engine_handle_t engine, sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_session_begin(sb_engine_handle_t engine,
                        const sb_engine_session_params_v1_t* params,
                        sb_engine_session_t* out_session,
                        sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_session_end(sb_engine_session_t session,
                      const sb_engine_session_end_params_v1_t* params,
                      sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_transaction_begin(sb_engine_session_t session,
                            const sb_engine_transaction_params_v1_t* params,
                            sb_engine_transaction_t* out_transaction,
                            sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_transaction_commit(sb_engine_transaction_t transaction,
                             const sb_engine_transaction_finish_params_v1_t* params,
                             sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_transaction_rollback(sb_engine_transaction_t transaction,
                               const sb_engine_transaction_finish_params_v1_t* params,
                               sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_dispatch_sblr(sb_engine_session_t session,
                        sb_engine_transaction_t transaction,
                        const sb_engine_request_context_v1_t* context,
                        const sb_engine_sblr_dispatch_params_v1_t* params,
                        sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_release(sb_engine_result_t result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_class(sb_engine_result_t result, sb_engine_result_class_t* out_class);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_completion(sb_engine_result_t result,
                            sb_engine_command_completion_view_v1_t* out_view);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_summary(sb_engine_result_t result,
                         sb_engine_execution_summary_view_v1_t* out_view);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_diagnostics(sb_engine_result_t result,
                             sb_engine_diagnostic_set_view_t* out_view);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_payload(sb_engine_result_t result, sb_engine_string_view_t* out_view);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_result_next_batch(sb_engine_result_t result,
                            const sb_engine_batch_request_v1_t* request,
                            sb_engine_row_batch_view_v1_t* out_batch);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_describe_capabilities(sb_engine_handle_t engine,
                                const sb_engine_capability_request_v1_t* request,
                                sb_engine_result_t* out_result);

SCRATCHBIRD_ENGINE_API sb_engine_status_t SCRATCHBIRD_ENGINE_CALL
sb_engine_metric_root(sb_engine_handle_t engine,
                      const sb_engine_metric_request_v1_t* request,
                      sb_engine_result_t* out_result);

#ifdef __cplusplus
}
#endif

#endif
