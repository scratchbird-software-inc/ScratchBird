// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/blob_lifetime_abi.h"
#include "scratchbird/engine/blob_lifetime_abi.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#  include <type_traits>
#  define ABI_ASSERT(condition) static_assert((condition), #condition)
#  define ABI_ALIGNOF(type) alignof(type)
#  define ABI_CHECK_TYPE(expression, type) \
    static_assert(std::is_same_v< \
                      std::remove_reference_t<decltype(expression)>, type>, \
                  #expression " type")
#  define ABI_CHECK_ARRAY(expression, element_type, extent) \
    static_assert(std::is_same_v< \
                      std::remove_extent_t<std::remove_reference_t< \
                          decltype(expression)>>, element_type>, \
                  #expression " element type"); \
    static_assert(std::extent_v<std::remove_reference_t< \
                      decltype(expression)>> == (extent), \
                  #expression " extent")
#  define ABI_CHECK_RECORD(type) \
    static_assert(std::is_standard_layout_v<type> && \
                      std::is_trivially_copyable_v<type>, \
                  #type " C layout")
#else
#  define ABI_ASSERT(condition) _Static_assert((condition), #condition)
#  define ABI_ALIGNOF(type) _Alignof(type)
#  define ABI_CHECK_TYPE(expression, type) \
    _Static_assert(_Generic((expression), type: 1, default: 0), \
                   #expression " type")
#  define ABI_CHECK_ARRAY(expression, element_type, extent) \
    _Static_assert(_Generic(&(expression), \
                            element_type (*)[extent]: 1, default: 0), \
                   #expression " element type and array type"); \
    _Static_assert(sizeof(expression) / sizeof(element_type) == (extent), \
                   #expression " extent")
#  define ABI_CHECK_RECORD(type) _Static_assert(1, #type " C layout")
#endif

#define ABI_CHECK_LAYOUT(type, size, alignment) \
  ABI_ASSERT(sizeof(type) == (size)); \
  ABI_ASSERT(ABI_ALIGNOF(type) == (alignment))
#define ABI_CHECK_OFFSET(type, field, offset) \
  ABI_ASSERT(offsetof(type, field) == (offset))
#define ABI_CHECK_VALUE(name, value) ABI_ASSERT((name) == (value))

ABI_CHECK_LAYOUT(BlobUuid16V3, 16, 1);
ABI_CHECK_OFFSET(BlobUuid16V3, bytes, 0);
ABI_CHECK_ARRAY(((BlobUuid16V3*)0)->bytes, uint8_t, 16);

ABI_CHECK_LAYOUT(BlobLifetimeTokenV3, 200, 8);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, struct_bytes, 0);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, abi_major, 4);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, abi_minor, 6);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, lifetime_token_uuid, 8);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, lifetime_token_generation, 24);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, owner_class, 32);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, reserved_33_39, 33);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, authority_instance_uuid, 40);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, authority_instance_generation, 56);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, database_uuid, 64);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, session_uuid, 80);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, statement_uuid, 96);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, transaction_uuid, 112);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, snapshot_uuid, 128);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, snapshot_generation, 144);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, security_context_uuid, 152);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, security_generation, 168);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, immutable_binding_generation, 176);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, valid_until_monotonic_ns, 184);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, mode_bits, 192);
ABI_CHECK_OFFSET(BlobLifetimeTokenV3, reserved_196_199, 196);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->struct_bytes, uint32_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->abi_major, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->abi_minor, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->lifetime_token_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->lifetime_token_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->owner_class, uint8_t);
ABI_CHECK_ARRAY(((BlobLifetimeTokenV3*)0)->reserved_33_39, uint8_t, 7);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->authority_instance_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->authority_instance_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->database_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->session_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->statement_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->transaction_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->snapshot_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->snapshot_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->security_context_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->security_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->immutable_binding_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->valid_until_monotonic_ns, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeTokenV3*)0)->mode_bits, uint32_t);
ABI_CHECK_ARRAY(((BlobLifetimeTokenV3*)0)->reserved_196_199, uint8_t, 4);

ABI_CHECK_LAYOUT(BlobLifetimeUseRequestV3, 160, 8);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, struct_bytes, 0);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, abi_major, 4);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, abi_minor, 6);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, operation, 8);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, required_owner_class, 9);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, reserved_10_11, 10);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, required_mode_bits, 12);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3,
                 expected_authority_instance_uuid, 16);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3,
                 expected_authority_instance_generation, 32);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_database_uuid, 40);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_session_uuid, 56);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_statement_uuid, 72);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_transaction_uuid, 88);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_snapshot_uuid, 104);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_snapshot_generation, 120);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3,
                 expected_security_context_uuid, 128);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3, expected_security_generation, 144);
ABI_CHECK_OFFSET(BlobLifetimeUseRequestV3,
                 absolute_deadline_monotonic_ns, 152);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->struct_bytes, uint32_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->abi_major, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->abi_minor, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->operation, uint8_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->required_owner_class, uint8_t);
ABI_CHECK_ARRAY(((BlobLifetimeUseRequestV3*)0)->reserved_10_11, uint8_t, 2);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->required_mode_bits, uint32_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_authority_instance_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_authority_instance_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_database_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_session_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_statement_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_transaction_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_snapshot_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_snapshot_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_security_context_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->expected_security_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeUseRequestV3*)0)->absolute_deadline_monotonic_ns, uint64_t);

ABI_CHECK_LAYOUT(BlobLifetimeRetainTicketV3, 48, 8);
ABI_CHECK_OFFSET(BlobLifetimeRetainTicketV3, retain_ticket_uuid, 0);
ABI_CHECK_OFFSET(BlobLifetimeRetainTicketV3, lifetime_token_uuid, 16);
ABI_CHECK_OFFSET(BlobLifetimeRetainTicketV3, lifetime_token_generation, 32);
ABI_CHECK_OFFSET(BlobLifetimeRetainTicketV3,
                 immutable_binding_generation, 40);
ABI_CHECK_TYPE(((BlobLifetimeRetainTicketV3*)0)->retain_ticket_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeRetainTicketV3*)0)->lifetime_token_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeRetainTicketV3*)0)->lifetime_token_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeRetainTicketV3*)0)->immutable_binding_generation, uint64_t);

ABI_CHECK_LAYOUT(BlobLifetimeAccessTicketV3, 48, 8);
ABI_CHECK_OFFSET(BlobLifetimeAccessTicketV3, access_ticket_uuid, 0);
ABI_CHECK_OFFSET(BlobLifetimeAccessTicketV3, retain_ticket_uuid, 16);
ABI_CHECK_OFFSET(BlobLifetimeAccessTicketV3, lifetime_token_generation, 32);
ABI_CHECK_OFFSET(BlobLifetimeAccessTicketV3,
                 immutable_binding_generation, 40);
ABI_CHECK_TYPE(((BlobLifetimeAccessTicketV3*)0)->access_ticket_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeAccessTicketV3*)0)->retain_ticket_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeAccessTicketV3*)0)->lifetime_token_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeAccessTicketV3*)0)->immutable_binding_generation, uint64_t);

ABI_CHECK_LAYOUT(BlobLifetimeCallbackResultV3, 8, 1);
ABI_CHECK_OFFSET(BlobLifetimeCallbackResultV3, code, 0);
ABI_CHECK_OFFSET(BlobLifetimeCallbackResultV3, reserved_1_7, 1);
ABI_CHECK_TYPE(((BlobLifetimeCallbackResultV3*)0)->code, uint8_t);
ABI_CHECK_ARRAY(((BlobLifetimeCallbackResultV3*)0)->reserved_1_7, uint8_t, 7);

ABI_CHECK_LAYOUT(BlobLifetimeAuthorityV3, 80, 8);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, struct_bytes, 0);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, abi_major, 4);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, abi_minor, 6);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, authority_instance_uuid, 8);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3,
                 authority_instance_generation, 24);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, authority_context, 32);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, retain, 40);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, probe, 48);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, begin_access, 56);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, end_access, 64);
ABI_CHECK_OFFSET(BlobLifetimeAuthorityV3, release, 72);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->struct_bytes, uint32_t);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->abi_major, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->abi_minor, uint16_t);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->authority_instance_uuid, BlobUuid16V3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->authority_instance_generation, uint64_t);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->authority_context, void*);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->retain, BlobLifetimeRetainFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->probe, BlobLifetimeProbeFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->begin_access, BlobLifetimeBeginAccessFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->end_access, BlobLifetimeEndAccessFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->release, BlobLifetimeReleaseFnV3);

ABI_CHECK_VALUE(SB_BLOB_LIFETIME_ABI_MAJOR_V3, 3);
ABI_CHECK_VALUE(SB_BLOB_LIFETIME_ABI_MINOR_V3, 0);
ABI_CHECK_VALUE(SB_BLOB_LIFETIME_ABI_GENERATION_V3, 1);

ABI_CHECK_VALUE(SB_BLOB_OWNER_EXECUTION_BORROW_V3, 1);
ABI_CHECK_VALUE(SB_BLOB_OWNER_DATATYPE_OWNED_VALUE_V3, 2);
ABI_CHECK_VALUE(SB_BLOB_OWNER_ENCODED_ARTIFACT_BORROW_V3, 3);

ABI_CHECK_VALUE(SB_BLOB_OP_VALIDATE_VIEW_V3, 1);
ABI_CHECK_VALUE(SB_BLOB_OP_READ_V3, 2);
ABI_CHECK_VALUE(SB_BLOB_OP_REPLAY_V3, 3);
ABI_CHECK_VALUE(SB_BLOB_OP_MATERIALIZE_V3, 4);
ABI_CHECK_VALUE(SB_BLOB_OP_HASH_V3, 5);
ABI_CHECK_VALUE(SB_BLOB_OP_COMPARE_V3, 6);
ABI_CHECK_VALUE(SB_BLOB_OP_INTRINSIC_TRANSFORM_V3, 7);
ABI_CHECK_VALUE(SB_BLOB_OP_COMPONENT_ENCODE_V3, 8);
ABI_CHECK_VALUE(SB_BLOB_OP_COMPONENT_DECODE_V3, 9);
ABI_CHECK_VALUE(SB_BLOB_OP_BACKUP_ENCODE_V3, 10);
ABI_CHECK_VALUE(SB_BLOB_OP_BACKUP_DECODE_V3, 11);
ABI_CHECK_VALUE(SB_BLOB_OP_PROTECT_V3, 12);
ABI_CHECK_VALUE(SB_BLOB_OP_RESTORE_V3, 13);

ABI_CHECK_VALUE(SB_BLOB_CALLBACK_OK_V3, 0);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_INVALID_ARGUMENT_V3, 1);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_UNKNOWN_TOKEN_V3, 2);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_WRONG_AUTHORITY_V3, 3);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_SECURITY_DENIED_V3, 4);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_WRONG_OWNER_V3, 5);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_WRONG_TRANSACTION_V3, 6);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_WRONG_SNAPSHOT_V3, 7);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_STALE_SNAPSHOT_V3, 8);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_EXPIRED_V3, 9);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_REVOKED_V3, 10);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_WRONG_MODE_V3, 11);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_CLOSED_V3, 12);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_RETAINED_TICKET_INVALID_V3, 13);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_RETAINED_TICKET_CONSUMED_V3, 14);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_ACCESS_TICKET_INVALID_V3, 15);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_ACCESS_TICKET_CONSUMED_V3, 16);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_RESOURCE_EXHAUSTED_V3, 17);
ABI_CHECK_VALUE(SB_BLOB_CALLBACK_AUTHORITY_UNAVAILABLE_V3, 18);

ABI_CHECK_VALUE(SB_BLOB_MODE_IMMUTABLE_READ_V3, UINT32_C(1));
ABI_CHECK_VALUE(SB_BLOB_MODE_REPLAYABLE_SAME_SNAPSHOT_V3, UINT32_C(2));
ABI_CHECK_VALUE(SB_BLOB_MODE_CONCURRENT_IMMUTABLE_READ_V3, UINT32_C(4));
ABI_CHECK_VALUE(SB_BLOB_MODE_MATERIALIZED_BYTES_V3, UINT32_C(8));
ABI_CHECK_VALUE(SB_BLOB_MODE_LOGICAL_READER_V3, UINT32_C(16));
ABI_CHECK_VALUE(SB_BLOB_MODE_OPAQUE_ENCODED_READER_V3, UINT32_C(32));
ABI_CHECK_VALUE(SB_BLOB_MODE_KNOWN_MASK_V3, UINT32_C(63));

static BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL abi_retain(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeUseRequestV3* request,
    BlobLifetimeRetainTicketV3* out_ticket) {
  BlobLifetimeCallbackResultV3 result = {SB_BLOB_CALLBACK_OK_V3, {0}};
  (void)context;
  (void)token;
  (void)request;
  (void)out_ticket;
  return result;
}

static BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL abi_probe(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    const BlobLifetimeUseRequestV3* request) {
  BlobLifetimeCallbackResultV3 result = {SB_BLOB_CALLBACK_OK_V3, {0}};
  (void)context;
  (void)token;
  (void)retain_ticket;
  (void)request;
  return result;
}

static BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL abi_begin_access(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    const BlobLifetimeUseRequestV3* request,
    BlobLifetimeAccessTicketV3* out_ticket) {
  BlobLifetimeCallbackResultV3 result = {SB_BLOB_CALLBACK_OK_V3, {0}};
  (void)context;
  (void)token;
  (void)retain_ticket;
  (void)request;
  (void)out_ticket;
  return result;
}

static BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL abi_end_access(
    void* context, const BlobLifetimeTokenV3* token,
    const BlobLifetimeRetainTicketV3* retain_ticket,
    BlobLifetimeAccessTicketV3* inout_ticket,
    const BlobLifetimeUseRequestV3* request) {
  BlobLifetimeCallbackResultV3 result = {SB_BLOB_CALLBACK_OK_V3, {0}};
  (void)context;
  (void)token;
  (void)retain_ticket;
  (void)inout_ticket;
  (void)request;
  return result;
}

static BlobLifetimeCallbackResultV3 SCRATCHBIRD_ENGINE_CALL abi_release(
    void* context, const BlobLifetimeTokenV3* token,
    BlobLifetimeRetainTicketV3* inout_ticket) {
  BlobLifetimeCallbackResultV3 result = {SB_BLOB_CALLBACK_OK_V3, {0}};
  (void)context;
  (void)token;
  (void)inout_ticket;
  return result;
}

ABI_CHECK_TYPE(&abi_retain, BlobLifetimeRetainFnV3);
ABI_CHECK_TYPE(&abi_probe, BlobLifetimeProbeFnV3);
ABI_CHECK_TYPE(&abi_begin_access, BlobLifetimeBeginAccessFnV3);
ABI_CHECK_TYPE(&abi_end_access, BlobLifetimeEndAccessFnV3);
ABI_CHECK_TYPE(&abi_release, BlobLifetimeReleaseFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->retain,
               BlobLifetimeRetainFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->probe,
               BlobLifetimeProbeFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->begin_access,
               BlobLifetimeBeginAccessFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->end_access,
               BlobLifetimeEndAccessFnV3);
ABI_CHECK_TYPE(((BlobLifetimeAuthorityV3*)0)->release,
               BlobLifetimeReleaseFnV3);

ABI_CHECK_RECORD(BlobUuid16V3);
ABI_CHECK_RECORD(BlobLifetimeTokenV3);
ABI_CHECK_RECORD(BlobLifetimeUseRequestV3);
ABI_CHECK_RECORD(BlobLifetimeRetainTicketV3);
ABI_CHECK_RECORD(BlobLifetimeAccessTicketV3);
ABI_CHECK_RECORD(BlobLifetimeCallbackResultV3);
ABI_CHECK_RECORD(BlobLifetimeAuthorityV3);

int main(void) {
  BlobLifetimeAuthorityV3 authority = {
      sizeof(BlobLifetimeAuthorityV3),
      SB_BLOB_LIFETIME_ABI_MAJOR_V3,
      SB_BLOB_LIFETIME_ABI_MINOR_V3,
      {{0}},
      UINT64_C(1),
      0,
      &abi_retain,
      &abi_probe,
      &abi_begin_access,
      &abi_end_access,
      &abi_release};
  return authority.retain == &abi_retain &&
                 authority.probe == &abi_probe &&
                 authority.begin_access == &abi_begin_access &&
                 authority.end_access == &abi_end_access &&
                 authority.release == &abi_release
             ? 0
             : 1;
}
