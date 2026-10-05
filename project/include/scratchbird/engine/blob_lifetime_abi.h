// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#ifndef SCRATCHBIRD_ENGINE_BLOB_LIFETIME_ABI_H
#define SCRATCHBIRD_ENGINE_BLOB_LIFETIME_ABI_H

/* ABI v3 generation 1 is published only for the x86-64 SysV and Win64
 * profiles. Refuse compilation when the selected target is outside them. */
#if !defined(__x86_64__) && !defined(_M_X64)
#  error "base.blob lifetime ABI v3 generation 1 requires x86-64"
#endif

#if defined(__BYTE_ORDER__)
#  if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#    error "base.blob lifetime ABI v3 generation 1 requires little endian"
#  endif
#elif !defined(_WIN32)
#  error "base.blob lifetime ABI v3 generation 1 cannot verify little endian"
#endif

#include "scratchbird/engine/export.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#  define SB_BLOB_LIFETIME_STATIC_ASSERT(condition, message) \
    static_assert((condition), message)
#  define SB_BLOB_LIFETIME_ALIGNOF(type) alignof(type)
extern "C" {
#else
#  define SB_BLOB_LIFETIME_STATIC_ASSERT(condition, message) \
    _Static_assert((condition), message)
#  define SB_BLOB_LIFETIME_ALIGNOF(type) _Alignof(type)
#endif

#define SB_BLOB_LIFETIME_ABI_MAJOR_V3 UINT16_C(3)
#define SB_BLOB_LIFETIME_ABI_MINOR_V3 UINT16_C(0)
#define SB_BLOB_LIFETIME_ABI_GENERATION_V3 UINT32_C(1)

typedef struct BlobUuid16V3 {
  uint8_t bytes[16];
} BlobUuid16V3;

typedef enum BlobLifetimeOwnerClassCodeV3 {
  SB_BLOB_OWNER_EXECUTION_BORROW_V3 = 1,
  SB_BLOB_OWNER_DATATYPE_OWNED_VALUE_V3 = 2,
  SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3 = 3
} BlobLifetimeOwnerClassCodeV3;

typedef enum BlobLifetimeOperationCodeV3 {
  SB_BLOB_OP_VALIDATE_VIEW_V3 = 1,
  SB_BLOB_OP_READ_V3 = 2,
  SB_BLOB_OP_REPLAY_V3 = 3,
  SB_BLOB_OP_MATERIALIZE_V3 = 4,
  SB_BLOB_OP_HASH_V3 = 5,
  SB_BLOB_OP_COMPARE_V3 = 6,
  SB_BLOB_OP_INTRINSIC_TRANSFORM_V3 = 7,
  SB_BLOB_OP_COMPONENT_ENCODE_V3 = 8,
  SB_BLOB_OP_COMPONENT_DECODE_V3 = 9,
  SB_BLOB_OP_BACKUP_ENCODE_V3 = 10,
  SB_BLOB_OP_BACKUP_DECODE_V3 = 11,
  SB_BLOB_OP_PROTECT_V3 = 12,
  SB_BLOB_OP_RESTORE_V3 = 13
} BlobLifetimeOperationCodeV3;

typedef enum BlobLifetimeCallbackCodeV3 {
  SB_BLOB_CALLBACK_OK_V3 = 0,
  SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3 = 1,
  SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3 = 2,
  SB_BLOB_CALLBACK_WRONG_AUTHORITY_V3 = 3,
  SB_BLOB_CALLBACK_SECURITY_DENIED_V3 = 4,
  SB_BLOB_CALLBACK_WRONG_OWNER_V3 = 5,
  SB_BLOB_CALLBACK_WRONG_TRANSACTION_V3 = 6,
  SB_BLOB_CALLBACK_WRONG_SNAPSHOT_V3 = 7,
  SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3 = 8,
  SB_BLOB_CALLBACK_EXPIRED_V3 = 9,
  SB_BLOB_CALLBACK_REVOKED_V3 = 10,
  SB_BLOB_CALLBACK_WRONG_MODE_V3 = 11,
  SB_BLOB_CALLBACK_CLOSED_V3 = 12,
  SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3 = 13,
  SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3 = 14,
  SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3 = 15,
  SB_BLOB_CALLBACK_ACCESS_TICKET_CONSUMED_V3 = 16,
  SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3 = 17,
  SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3 = 18
} BlobLifetimeCallbackCodeV3;

enum BlobLifetimeModeBitsV3 {
  SB_BLOB_MODE_IMMUTABLE_READ_V3 = UINT32_C(0x00000001),
  SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3 = UINT32_C(0x00000002),
  SB_BLOB_MODE_CONCURRENT_IMMUTABLE_READ_V3 = UINT32_C(0x00000004),
  SB_BLOB_MODE_MATERIALIZED_BYTES_V3 = UINT32_C(0x00000008),
  SB_BLOB_MODE_LOGICAL_READER_V3 = UINT32_C(0x00000010),
  SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3 = UINT32_C(0x00000020),
  SB_BLOB_MODE_KNOWN_MASK_V3 = UINT32_C(0x0000003f)
};

typedef struct BlobLifetimeTokenV3 {
  uint32_t struct_bytes;
  uint16_t abi_major;
  uint16_t abi_minor;
  BlobUuid16V3 lifetime_token_uuid;
  uint64_t lifetime_token_generation;
  uint8_t owner_class;
  uint8_t reserved_33_39[7];
  BlobUuid16V3 authority_instance_uuid;
  uint64_t authority_instance_generation;
  BlobUuid16V3 database_uuid;
  BlobUuid16V3 session_uuid;
  BlobUuid16V3 statement_uuid;
  BlobUuid16V3 transaction_uuid;
  BlobUuid16V3 snapshot_uuid;
  uint64_t snapshot_generation;
  BlobUuid16V3 security_context_uuid;
  uint64_t security_generation;
  uint64_t immutable_binding_generation;
  uint64_t valid_until_monotonic_ns;
  uint32_t mode_bits;
  uint8_t reserved_196_199[4];
} BlobLifetimeTokenV3;

typedef struct BlobLifetimeUseRequestV3 {
  uint32_t struct_bytes;
  uint16_t abi_major;
  uint16_t abi_minor;
  uint8_t operation;
  uint8_t required_owner_class;
  uint8_t reserved_10_11[2];
  uint32_t required_mode_bits;
  BlobUuid16V3 expected_authority_instance_uuid;
  uint64_t expected_authority_instance_generation;
  BlobUuid16V3 expected_database_uuid;
  BlobUuid16V3 expected_session_uuid;
  BlobUuid16V3 expected_statement_uuid;
  BlobUuid16V3 expected_transaction_uuid;
  BlobUuid16V3 expected_snapshot_uuid;
  uint64_t expected_snapshot_generation;
  BlobUuid16V3 expected_security_context_uuid;
  uint64_t expected_security_generation;
  uint64_t absolute_deadline_monotonic_ns;
} BlobLifetimeUseRequestV3;

typedef struct BlobLifetimeRetainTicketV3 {
  BlobUuid16V3 retain_ticket_uuid;
  BlobUuid16V3 lifetime_token_uuid;
  uint64_t lifetime_token_generation;
  uint64_t immutable_binding_generation;
} BlobLifetimeRetainTicketV3;

typedef struct BlobLifetimeAccessTicketV3 {
  BlobUuid16V3 access_ticket_uuid;
  BlobUuid16V3 retain_ticket_uuid;
  uint64_t lifetime_token_generation;
  uint64_t immutable_binding_generation;
} BlobLifetimeAccessTicketV3;

typedef struct BlobLifetimeCallbackResultV3 {
  uint8_t code;
  uint8_t reserved_1_7[7];
} BlobLifetimeCallbackResultV3;

typedef BlobLifetimeCallbackResultV3 (
    SCRATCHBIRD_ENGINE_CALL *BlobLifetimeRetainFnV3)(
    void* authority_context,
    const BlobLifetimeTokenV3* token,
    const BlobLifetimeUseRequestV3* request,
    BlobLifetimeRetainTicketV3* out_ticket);

typedef BlobLifetimeCallbackResultV3 (
    SCRATCHBIRD_ENGINE_CALL *BlobLifetimeProbeFnV3)(
    void* authority_context,
    const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    const BlobLifetimeUseRequestV3* request);

typedef BlobLifetimeCallbackResultV3 (
    SCRATCHBIRD_ENGINE_CALL *BlobLifetimeBeginAccessFnV3)(
    void* authority_context,
    const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    const BlobLifetimeUseRequestV3* request,
    BlobLifetimeAccessTicketV3* out_ticket);

typedef BlobLifetimeCallbackResultV3 (
    SCRATCHBIRD_ENGINE_CALL *BlobLifetimeEndAccessFnV3)(
    void* authority_context,
    const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    BlobLifetimeAccessTicketV3* inout_ticket,
    const BlobLifetimeUseRequestV3* request);

typedef BlobLifetimeCallbackResultV3 (
    SCRATCHBIRD_ENGINE_CALL *BlobLifetimeReleaseFnV3)(
    void* authority_context,
    const BlobLifetimeTokenV3* token,
    BlobLifetimeRetainTicketV3* inout_ticket);

typedef struct BlobLifetimeAuthorityV3 {
  uint32_t struct_bytes;
  uint16_t abi_major;
  uint16_t abi_minor;
  BlobUuid16V3 authority_instance_uuid;
  uint64_t authority_instance_generation;
  void* authority_context;
  BlobLifetimeRetainFnV3 retain;
  BlobLifetimeProbeFnV3 probe;
  BlobLifetimeBeginAccessFnV3 begin_access;
  BlobLifetimeEndAccessFnV3 end_access;
  BlobLifetimeReleaseFnV3 release;
} BlobLifetimeAuthorityV3;

SB_BLOB_LIFETIME_STATIC_ASSERT(CHAR_BIT == 8, "base.blob lifetime ABI requires 8-bit bytes");
SB_BLOB_LIFETIME_STATIC_ASSERT(INT8_MIN == (-INT8_MAX - 1), "base.blob lifetime ABI requires two's-complement int8_t");
SB_BLOB_LIFETIME_STATIC_ASSERT(INT16_MIN == (-INT16_MAX - 1), "base.blob lifetime ABI requires two's-complement int16_t");
SB_BLOB_LIFETIME_STATIC_ASSERT(INT32_MIN == (-INT32_MAX - 1), "base.blob lifetime ABI requires two's-complement int32_t");
SB_BLOB_LIFETIME_STATIC_ASSERT(INT64_MIN == (-INT64_MAX - 1), "base.blob lifetime ABI requires two's-complement int64_t");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(void*) == 8, "base.blob lifetime ABI requires 64-bit data pointers");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeRetainFnV3) == 8, "base.blob lifetime ABI requires 64-bit function pointers");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(uint64_t) == 8, "base.blob lifetime ABI requires 64-bit uint64_t");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobUuid16V3) == 16, "BlobUuid16V3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobUuid16V3) == 1, "BlobUuid16V3 alignment");

SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeTokenV3) == 200, "BlobLifetimeTokenV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeTokenV3) == 8, "BlobLifetimeTokenV3 alignment");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeUseRequestV3) == 160, "BlobLifetimeUseRequestV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeUseRequestV3) == 8, "BlobLifetimeUseRequestV3 alignment");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeRetainTicketV3) == 48, "BlobLifetimeRetainTicketV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeRetainTicketV3) == 8, "BlobLifetimeRetainTicketV3 alignment");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeAccessTicketV3) == 48, "BlobLifetimeAccessTicketV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeAccessTicketV3) == 8, "BlobLifetimeAccessTicketV3 alignment");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeCallbackResultV3) == 8, "BlobLifetimeCallbackResultV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeCallbackResultV3) == 1, "BlobLifetimeCallbackResultV3 alignment");
SB_BLOB_LIFETIME_STATIC_ASSERT(sizeof(BlobLifetimeAuthorityV3) == 80, "BlobLifetimeAuthorityV3 size");
SB_BLOB_LIFETIME_STATIC_ASSERT(SB_BLOB_LIFETIME_ALIGNOF(BlobLifetimeAuthorityV3) == 8, "BlobLifetimeAuthorityV3 alignment");

#ifdef __cplusplus
}  // extern "C"
#endif

#undef SB_BLOB_LIFETIME_ALIGNOF
#undef SB_BLOB_LIFETIME_STATIC_ASSERT

#endif
