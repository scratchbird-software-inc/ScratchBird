// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_CLUSTER_GATEWAY_ABI_V1_H
#define SCRATCHBIRD_CLUSTER_GATEWAY_ABI_V1_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#if defined(__cplusplus)
extern "C" {
# define SB_CG_STATIC_ASSERT(expr, message) static_assert((expr), message)
# define SB_CG_ALIGNOF(type) alignof(type)
#else
# define SB_CG_STATIC_ASSERT(expr, message) _Static_assert((expr), message)
# define SB_CG_ALIGNOF(type) _Alignof(type)
#endif

#if defined(_WIN32)
# define SB_CG_CALL __cdecl
# if defined(SB_CLUSTER_GATEWAY_BUILD)
#  define SB_CG_API __declspec(dllexport)
# else
#  define SB_CG_API __declspec(dllimport)
# endif
#else
# define SB_CG_CALL
# define SB_CG_API __attribute__((visibility("default")))
#endif

#define SB_CG_ABI_MAJOR_V1 UINT16_C(1)
#define SB_CG_ABI_MINOR_V1 UINT16_C(0)
#define SB_CG_SHA256_BYTES UINT32_C(32)
#define SB_CG_UUID_BYTES UINT32_C(16)
#define SB_CG_MAX_ENVELOPE_BYTES UINT64_C(16777216)
#define SB_CG_MAX_HOST_CALL_BYTES UINT64_C(1048576)

typedef struct SbCgSessionV1 SbCgSessionV1;

typedef struct SbCgUuidV1 { uint8_t bytes[16]; } SbCgUuidV1;
typedef struct SbCgDigestV1 { uint8_t bytes[32]; } SbCgDigestV1;
typedef struct SbCgAbiHeaderV1 {
    uint32_t struct_bytes;
    uint16_t abi_major;
    uint16_t abi_minor;
} SbCgAbiHeaderV1;
typedef struct SbCgByteViewV1 {
    const uint8_t *data;
    uint64_t bytes;
} SbCgByteViewV1;
typedef struct SbCgMutableBufferV1 {
    uint8_t *data;
    uint64_t capacity_bytes;
    uint64_t written_bytes;
} SbCgMutableBufferV1;

typedef int32_t SbCgStatusV1;
enum {
    SB_CG_STATUS_OK = 0,
    SB_CG_STATUS_BUFFER_TOO_SMALL = 1,
    SB_CG_STATUS_REFUSED = 2,
    SB_CG_STATUS_RETRYABLE = 3,
    SB_CG_STATUS_INVALID_ARGUMENT = -1,
    SB_CG_STATUS_ABI_INCOMPATIBLE = -2,
    SB_CG_STATUS_PROVIDER_UNAVAILABLE = -3,
    SB_CG_STATUS_RESULT_INVALID = -4,
    SB_CG_STATUS_REENTRANCY_FORBIDDEN = -5,
    SB_CG_STATUS_INTERNAL_FAILURE = -6
};

enum { SB_CG_GATEWAY_KIND_COMPILE_LINK_STUB = 1, SB_CG_GATEWAY_KIND_PRIVATE_PROVIDER = 2 };
enum { SB_CG_EDITION_STANDALONE_PUBLIC = 1, SB_CG_EDITION_CLUSTER_PRIVATE = 2 };
enum { SB_CG_REQUEST_LOCAL_ONLY = 1, SB_CG_REQUEST_CLUSTER_REQUIRED = 2 };
enum { SB_CG_DISPOSITION_PASS_THROUGH = 1, SB_CG_DISPOSITION_HANDLED = 2,
       SB_CG_DISPOSITION_ASYNC_ACCEPTED = 3, SB_CG_DISPOSITION_REFUSED = 4 };
enum { SB_CG_CANCEL_NONE = 0, SB_CG_CANCEL_PRE_EFFECT = 1,
       SB_CG_CANCEL_POST_PREPARE = 2, SB_CG_CANCEL_COMPLETED = 3 };
enum { SB_CG_RETRY_NEVER = 0, SB_CG_RETRY_AFTER_STATE_CHANGE = 1,
       SB_CG_RETRY_WITH_SAME_IDEMPOTENCY = 2, SB_CG_RETRY_NEW_REQUEST = 3 };
enum { SB_CG_HOST_RESOLVE_CATALOG_SECURITY = 1, SB_CG_HOST_VALIDATE_ROUTE_FENCE = 2,
       SB_CG_HOST_RESERVE_OPERATION = 3, SB_CG_HOST_RECORD_EVIDENCE = 4,
       SB_CG_HOST_MGA_PREPARE_EFFECT = 5, SB_CG_HOST_MGA_COMMIT_OR_ROLLBACK_EFFECT = 6,
       SB_CG_HOST_READ_REDACTED_METRICS = 7, SB_CG_HOST_EMIT_AUDITED_DIAGNOSTIC = 8 };

typedef struct SbCgDescriptorV1 {
    SbCgAbiHeaderV1 header;
    SbCgUuidV1 provider_build_uuid;
    SbCgDigestV1 provider_manifest_digest;
    SbCgDigestV1 operation_profile_digest;
    SbCgDigestV1 capability_digest;
    SbCgUuidV1 signature_key_id;
    uint16_t abi_minor_min;
    uint16_t abi_minor_max;
    uint16_t sblr_wire_min;
    uint16_t sblr_wire_max;
    uint32_t gateway_kind;
    uint32_t edition;
    uint64_t capability_bits;
    uint64_t reserved0;
} SbCgDescriptorV1;

typedef struct SbCgHostCallRequestV1 {
    SbCgAbiHeaderV1 header;
    SbCgUuidV1 request_uuid;
    SbCgUuidV1 session_nonce;
    uint32_t call_kind;
    uint32_t flags;
    SbCgDigestV1 canonical_envelope_digest;
    SbCgByteViewV1 payload;
    uint64_t deadline_monotonic_ns;
    uint64_t reserved0;
} SbCgHostCallRequestV1;
typedef SbCgStatusV1 (SB_CG_CALL *SbCgHostInvokeV1)(
    void *host_context, const SbCgHostCallRequestV1 *request,
    SbCgMutableBufferV1 *reply);
typedef struct SbCgHostV1 {
    SbCgAbiHeaderV1 header;
    void *host_context;
    SbCgHostInvokeV1 invoke;
    uint64_t permitted_call_bits;
    uint64_t reserved0;
} SbCgHostV1;

typedef struct SbCgOpenParamsV1 {
    SbCgAbiHeaderV1 header;
    SbCgUuidV1 database_uuid;
    SbCgUuidV1 process_uuid;
    const SbCgHostV1 *host;
    uint32_t open_flags;
    uint32_t reserved0;
} SbCgOpenParamsV1;
typedef struct SbCgRequestV1 {
    SbCgAbiHeaderV1 header;
    SbCgUuidV1 request_uuid;
    SbCgUuidV1 database_uuid;
    SbCgUuidV1 cluster_uuid;
    uint16_t operation_code;
    uint16_t operation_family;
    uint8_t transaction_class;
    uint8_t request_classification;
    uint16_t flags;
    SbCgByteViewV1 canonical_envelope;
    SbCgDigestV1 canonical_envelope_digest;
    SbCgDigestV1 dependency_uuid_vector_digest;
    SbCgDigestV1 authority_vector_digest;
    SbCgUuidV1 idempotency_uuid;
    uint64_t deadline_monotonic_ns;
    uint32_t renderer_profile_id;
    uint32_t reserved0;
    SbCgUuidV1 session_nonce;
} SbCgRequestV1;
typedef struct SbCgResultV1 {
    SbCgAbiHeaderV1 header;
    SbCgUuidV1 request_uuid;
    SbCgDigestV1 canonical_envelope_digest;
    SbCgUuidV1 session_nonce;
    uint32_t disposition;
    SbCgStatusV1 status;
    uint32_t diagnostic_code;
    uint32_t retry_class;
    SbCgUuidV1 operation_uuid;
    SbCgByteViewV1 typed_result;
    SbCgDigestV1 typed_result_digest;
    SbCgByteViewV1 evidence_proposal;
    SbCgDigestV1 evidence_proposal_digest;
    uint32_t cancellation_boundary;
    uint32_t reserved0;
    uint64_t provider_observation_generation;
} SbCgResultV1;

SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_get_descriptor(
    SbCgDescriptorV1 *out_descriptor);
SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_open(
    const SbCgOpenParamsV1 *params, SbCgSessionV1 **out_session);
SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_dispatch(
    SbCgSessionV1 *session, const SbCgRequestV1 *request,
    SbCgMutableBufferV1 *result_storage, SbCgResultV1 *out_result);
SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_poll(
    SbCgSessionV1 *session, const SbCgUuidV1 *operation_uuid,
    SbCgMutableBufferV1 *result_storage, SbCgResultV1 *out_result);
SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_cancel(
    SbCgSessionV1 *session, const SbCgUuidV1 *operation_uuid,
    uint32_t cancel_flags, SbCgMutableBufferV1 *result_storage,
    SbCgResultV1 *out_result);
SB_CG_API SbCgStatusV1 SB_CG_CALL sb_cluster_gateway_abi_v1_close(
    SbCgSessionV1 *session);

SB_CG_STATIC_ASSERT(CHAR_BIT == 8, "SB_CG requires octets");
SB_CG_STATIC_ASSERT(sizeof(void *) == 8, "SB_CG requires 64-bit pointers");
SB_CG_STATIC_ASSERT(sizeof(SbCgUuidV1) == 16 && SB_CG_ALIGNOF(SbCgUuidV1) == 1,
                    "SbCgUuidV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgDigestV1) == 32 && SB_CG_ALIGNOF(SbCgDigestV1) == 1,
                    "SbCgDigestV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgAbiHeaderV1) == 8 && offsetof(SbCgAbiHeaderV1, abi_major) == 4,
                    "SbCgAbiHeaderV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgByteViewV1) == 16 && offsetof(SbCgByteViewV1, bytes) == 8,
                    "SbCgByteViewV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgMutableBufferV1) == 24 && offsetof(SbCgMutableBufferV1, written_bytes) == 16,
                    "SbCgMutableBufferV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgDescriptorV1) == 168 && offsetof(SbCgDescriptorV1, gateway_kind) == 144,
                    "SbCgDescriptorV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgHostCallRequestV1) == 112 && offsetof(SbCgHostCallRequestV1, payload) == 80,
                    "SbCgHostCallRequestV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgHostV1) == 40 && offsetof(SbCgHostV1, invoke) == 16,
                    "SbCgHostV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgOpenParamsV1) == 56 && offsetof(SbCgOpenParamsV1, host) == 40,
                    "SbCgOpenParamsV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgRequestV1) == 224 && offsetof(SbCgRequestV1, canonical_envelope) == 64 &&
                    offsetof(SbCgRequestV1, session_nonce) == 208, "SbCgRequestV1 layout");
SB_CG_STATIC_ASSERT(sizeof(SbCgResultV1) == 216 && offsetof(SbCgResultV1, typed_result) == 104 &&
                    offsetof(SbCgResultV1, provider_observation_generation) == 208, "SbCgResultV1 layout");

#if defined(__cplusplus)
}
#endif
#endif
