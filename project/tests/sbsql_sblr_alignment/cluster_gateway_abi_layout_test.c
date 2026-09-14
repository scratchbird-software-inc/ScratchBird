// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include <scratchbird/cluster_gateway_abi_v1.h>
#include <scratchbird/cluster_gateway_abi_v1.h> /* Header is idempotent. */
#include <stdio.h>

#if defined(__cplusplus)
#include <type_traits>
#define CHECK_TYPE(expression, type) \
    static_assert(std::is_same_v<std::remove_reference_t<decltype(expression)>, type>, #expression " type")
#define CHECK_RECORD(type) \
    static_assert(std::is_standard_layout_v<type> && \
                  std::is_trivially_copyable_v<type>, #type " C layout")
#else
#define CHECK_TYPE(expression, type) \
    _Static_assert(_Generic((expression), type: 1, default: 0), #expression " type")
#define CHECK_RECORD(type)
#endif

#define CHECK_LAYOUT(type, size, alignment) \
    SB_CG_STATIC_ASSERT(sizeof(type) == (size), #type " size"); \
    SB_CG_STATIC_ASSERT(SB_CG_ALIGNOF(type) == (alignment), #type " alignment")
#define CHECK_OFFSET(type, field, offset) \
    SB_CG_STATIC_ASSERT(offsetof(type, field) == (offset), #type "." #field " offset")
#define CHECK_VALUE(name, value) SB_CG_STATIC_ASSERT((name) == (value), #name " value")

CHECK_LAYOUT(SbCgUuidV1, 16, 1);
CHECK_LAYOUT(SbCgDigestV1, 32, 1);
CHECK_LAYOUT(SbCgAbiHeaderV1, 8, 4);
CHECK_LAYOUT(SbCgByteViewV1, 16, 8);
CHECK_LAYOUT(SbCgMutableBufferV1, 24, 8);
CHECK_LAYOUT(SbCgDescriptorV1, 168, 8);
CHECK_LAYOUT(SbCgHostCallRequestV1, 112, 8);
CHECK_LAYOUT(SbCgHostV1, 40, 8);
CHECK_LAYOUT(SbCgOpenParamsV1, 56, 8);
CHECK_LAYOUT(SbCgRequestV1, 224, 8);
CHECK_LAYOUT(SbCgResultV1, 216, 8);
CHECK_LAYOUT(SbCgStatusV1, 4, 4);

CHECK_OFFSET(SbCgUuidV1, bytes, 0);
CHECK_OFFSET(SbCgDigestV1, bytes, 0);
CHECK_OFFSET(SbCgAbiHeaderV1, struct_bytes, 0);
CHECK_OFFSET(SbCgAbiHeaderV1, abi_major, 4);
CHECK_OFFSET(SbCgAbiHeaderV1, abi_minor, 6);
CHECK_OFFSET(SbCgByteViewV1, data, 0);
CHECK_OFFSET(SbCgByteViewV1, bytes, 8);
CHECK_OFFSET(SbCgMutableBufferV1, data, 0);
CHECK_OFFSET(SbCgMutableBufferV1, capacity_bytes, 8);
CHECK_OFFSET(SbCgMutableBufferV1, written_bytes, 16);

CHECK_OFFSET(SbCgDescriptorV1, header, 0);
CHECK_OFFSET(SbCgDescriptorV1, provider_build_uuid, 8);
CHECK_OFFSET(SbCgDescriptorV1, provider_manifest_digest, 24);
CHECK_OFFSET(SbCgDescriptorV1, operation_profile_digest, 56);
CHECK_OFFSET(SbCgDescriptorV1, capability_digest, 88);
CHECK_OFFSET(SbCgDescriptorV1, signature_key_id, 120);
CHECK_OFFSET(SbCgDescriptorV1, abi_minor_min, 136);
CHECK_OFFSET(SbCgDescriptorV1, abi_minor_max, 138);
CHECK_OFFSET(SbCgDescriptorV1, sblr_wire_min, 140);
CHECK_OFFSET(SbCgDescriptorV1, sblr_wire_max, 142);
CHECK_OFFSET(SbCgDescriptorV1, gateway_kind, 144);
CHECK_OFFSET(SbCgDescriptorV1, edition, 148);
CHECK_OFFSET(SbCgDescriptorV1, capability_bits, 152);
CHECK_OFFSET(SbCgDescriptorV1, reserved0, 160);

CHECK_OFFSET(SbCgHostCallRequestV1, header, 0);
CHECK_OFFSET(SbCgHostCallRequestV1, request_uuid, 8);
CHECK_OFFSET(SbCgHostCallRequestV1, session_nonce, 24);
CHECK_OFFSET(SbCgHostCallRequestV1, call_kind, 40);
CHECK_OFFSET(SbCgHostCallRequestV1, flags, 44);
CHECK_OFFSET(SbCgHostCallRequestV1, canonical_envelope_digest, 48);
CHECK_OFFSET(SbCgHostCallRequestV1, payload, 80);
CHECK_OFFSET(SbCgHostCallRequestV1, deadline_monotonic_ns, 96);
CHECK_OFFSET(SbCgHostCallRequestV1, reserved0, 104);
CHECK_OFFSET(SbCgHostV1, header, 0);
CHECK_OFFSET(SbCgHostV1, host_context, 8);
CHECK_OFFSET(SbCgHostV1, invoke, 16);
CHECK_OFFSET(SbCgHostV1, permitted_call_bits, 24);
CHECK_OFFSET(SbCgHostV1, reserved0, 32);
CHECK_OFFSET(SbCgOpenParamsV1, header, 0);
CHECK_OFFSET(SbCgOpenParamsV1, database_uuid, 8);
CHECK_OFFSET(SbCgOpenParamsV1, process_uuid, 24);
CHECK_OFFSET(SbCgOpenParamsV1, host, 40);
CHECK_OFFSET(SbCgOpenParamsV1, open_flags, 48);
CHECK_OFFSET(SbCgOpenParamsV1, reserved0, 52);

CHECK_OFFSET(SbCgRequestV1, header, 0);
CHECK_OFFSET(SbCgRequestV1, request_uuid, 8);
CHECK_OFFSET(SbCgRequestV1, database_uuid, 24);
CHECK_OFFSET(SbCgRequestV1, cluster_uuid, 40);
CHECK_OFFSET(SbCgRequestV1, operation_code, 56);
CHECK_OFFSET(SbCgRequestV1, operation_family, 58);
CHECK_OFFSET(SbCgRequestV1, transaction_class, 60);
CHECK_OFFSET(SbCgRequestV1, request_classification, 61);
CHECK_OFFSET(SbCgRequestV1, flags, 62);
CHECK_OFFSET(SbCgRequestV1, canonical_envelope, 64);
CHECK_OFFSET(SbCgRequestV1, canonical_envelope_digest, 80);
CHECK_OFFSET(SbCgRequestV1, dependency_uuid_vector_digest, 112);
CHECK_OFFSET(SbCgRequestV1, authority_vector_digest, 144);
CHECK_OFFSET(SbCgRequestV1, idempotency_uuid, 176);
CHECK_OFFSET(SbCgRequestV1, deadline_monotonic_ns, 192);
CHECK_OFFSET(SbCgRequestV1, renderer_profile_id, 200);
CHECK_OFFSET(SbCgRequestV1, reserved0, 204);
CHECK_OFFSET(SbCgRequestV1, session_nonce, 208);

CHECK_OFFSET(SbCgResultV1, header, 0);
CHECK_OFFSET(SbCgResultV1, request_uuid, 8);
CHECK_OFFSET(SbCgResultV1, canonical_envelope_digest, 24);
CHECK_OFFSET(SbCgResultV1, session_nonce, 56);
CHECK_OFFSET(SbCgResultV1, disposition, 72);
CHECK_OFFSET(SbCgResultV1, status, 76);
CHECK_OFFSET(SbCgResultV1, diagnostic_code, 80);
CHECK_OFFSET(SbCgResultV1, retry_class, 84);
CHECK_OFFSET(SbCgResultV1, operation_uuid, 88);
CHECK_OFFSET(SbCgResultV1, typed_result, 104);
CHECK_OFFSET(SbCgResultV1, typed_result_digest, 120);
CHECK_OFFSET(SbCgResultV1, evidence_proposal, 152);
CHECK_OFFSET(SbCgResultV1, evidence_proposal_digest, 168);
CHECK_OFFSET(SbCgResultV1, cancellation_boundary, 200);
CHECK_OFFSET(SbCgResultV1, reserved0, 204);
CHECK_OFFSET(SbCgResultV1, provider_observation_generation, 208);

CHECK_VALUE(SB_CG_ABI_MAJOR_V1, 1);
CHECK_VALUE(SB_CG_ABI_MINOR_V1, 0);
CHECK_VALUE(SB_CG_SHA256_BYTES, 32);
CHECK_VALUE(SB_CG_UUID_BYTES, 16);
CHECK_VALUE(SB_CG_MAX_ENVELOPE_BYTES, 16777216);
CHECK_VALUE(SB_CG_MAX_HOST_CALL_BYTES, 1048576);
CHECK_VALUE(SB_CG_STATUS_OK, 0);
CHECK_VALUE(SB_CG_STATUS_BUFFER_TOO_SMALL, 1);
CHECK_VALUE(SB_CG_STATUS_REFUSED, 2);
CHECK_VALUE(SB_CG_STATUS_RETRYABLE, 3);
CHECK_VALUE(SB_CG_STATUS_INVALID_ARGUMENT, -1);
CHECK_VALUE(SB_CG_STATUS_ABI_INCOMPATIBLE, -2);
CHECK_VALUE(SB_CG_STATUS_PROVIDER_UNAVAILABLE, -3);
CHECK_VALUE(SB_CG_STATUS_RESULT_INVALID, -4);
CHECK_VALUE(SB_CG_STATUS_REENTRANCY_FORBIDDEN, -5);
CHECK_VALUE(SB_CG_STATUS_INTERNAL_FAILURE, -6);
CHECK_VALUE(SB_CG_GATEWAY_KIND_COMPILE_LINK_STUB, 1);
CHECK_VALUE(SB_CG_GATEWAY_KIND_PRIVATE_PROVIDER, 2);
CHECK_VALUE(SB_CG_EDITION_STANDALONE_PUBLIC, 1);
CHECK_VALUE(SB_CG_EDITION_CLUSTER_PRIVATE, 2);
CHECK_VALUE(SB_CG_REQUEST_LOCAL_ONLY, 1);
CHECK_VALUE(SB_CG_REQUEST_CLUSTER_REQUIRED, 2);
CHECK_VALUE(SB_CG_DISPOSITION_PASS_THROUGH, 1);
CHECK_VALUE(SB_CG_DISPOSITION_HANDLED, 2);
CHECK_VALUE(SB_CG_DISPOSITION_ASYNC_ACCEPTED, 3);
CHECK_VALUE(SB_CG_DISPOSITION_REFUSED, 4);
CHECK_VALUE(SB_CG_CANCEL_NONE, 0);
CHECK_VALUE(SB_CG_CANCEL_PRE_EFFECT, 1);
CHECK_VALUE(SB_CG_CANCEL_POST_PREPARE, 2);
CHECK_VALUE(SB_CG_CANCEL_COMPLETED, 3);
CHECK_VALUE(SB_CG_RETRY_NEVER, 0);
CHECK_VALUE(SB_CG_RETRY_AFTER_STATE_CHANGE, 1);
CHECK_VALUE(SB_CG_RETRY_WITH_SAME_IDEMPOTENCY, 2);
CHECK_VALUE(SB_CG_RETRY_NEW_REQUEST, 3);
CHECK_VALUE(SB_CG_HOST_RESOLVE_CATALOG_SECURITY, 1);
CHECK_VALUE(SB_CG_HOST_VALIDATE_ROUTE_FENCE, 2);
CHECK_VALUE(SB_CG_HOST_RESERVE_OPERATION, 3);
CHECK_VALUE(SB_CG_HOST_RECORD_EVIDENCE, 4);
CHECK_VALUE(SB_CG_HOST_MGA_PREPARE_EFFECT, 5);
CHECK_VALUE(SB_CG_HOST_MGA_COMMIT_OR_ROLLBACK_EFFECT, 6);
CHECK_VALUE(SB_CG_HOST_READ_REDACTED_METRICS, 7);
CHECK_VALUE(SB_CG_HOST_EMIT_AUDITED_DIAGNOSTIC, 8);

CHECK_TYPE(((SbCgUuidV1 *)0)->bytes[0], uint8_t);
CHECK_TYPE(((SbCgDigestV1 *)0)->bytes[0], uint8_t);
CHECK_TYPE(((SbCgByteViewV1 *)0)->data, const uint8_t *);
CHECK_TYPE(((SbCgMutableBufferV1 *)0)->data, uint8_t *);
CHECK_TYPE(((SbCgAbiHeaderV1 *)0)->struct_bytes, uint32_t);
CHECK_TYPE(((SbCgAbiHeaderV1 *)0)->abi_major, uint16_t);
CHECK_TYPE(((SbCgAbiHeaderV1 *)0)->abi_minor, uint16_t);
CHECK_TYPE(((SbCgRequestV1 *)0)->operation_code, uint16_t);
CHECK_TYPE(((SbCgRequestV1 *)0)->operation_family, uint16_t);
CHECK_TYPE(((SbCgRequestV1 *)0)->transaction_class, uint8_t);
CHECK_TYPE(((SbCgRequestV1 *)0)->request_classification, uint8_t);
CHECK_TYPE(((SbCgResultV1 *)0)->status, int32_t);

typedef SbCgStatusV1 (SB_CG_CALL *DescriptorFunction)(SbCgDescriptorV1 *);
typedef SbCgStatusV1 (SB_CG_CALL *OpenFunction)(const SbCgOpenParamsV1 *, SbCgSessionV1 **);
typedef SbCgStatusV1 (SB_CG_CALL *DispatchFunction)(
    SbCgSessionV1 *, const SbCgRequestV1 *, SbCgMutableBufferV1 *, SbCgResultV1 *);
typedef SbCgStatusV1 (SB_CG_CALL *PollFunction)(
    SbCgSessionV1 *, const SbCgUuidV1 *, SbCgMutableBufferV1 *, SbCgResultV1 *);
typedef SbCgStatusV1 (SB_CG_CALL *CancelFunction)(
    SbCgSessionV1 *, const SbCgUuidV1 *, uint32_t, SbCgMutableBufferV1 *, SbCgResultV1 *);
typedef SbCgStatusV1 (SB_CG_CALL *CloseFunction)(SbCgSessionV1 *);
typedef SbCgStatusV1 (SB_CG_CALL *HostFunction)(
    void *, const SbCgHostCallRequestV1 *, SbCgMutableBufferV1 *);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_get_descriptor, DescriptorFunction);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_open, OpenFunction);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_dispatch, DispatchFunction);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_poll, PollFunction);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_cancel, CancelFunction);
CHECK_TYPE(&sb_cluster_gateway_abi_v1_close, CloseFunction);
CHECK_TYPE(((SbCgHostV1 *)0)->invoke, HostFunction);

#if defined(SB_CG_SYMBOL_PROBE)
/* Compile only: no mock definitions or replacement implementation is linked.
 * Object inspection verifies the actual C symbol references in both languages. */
DescriptorFunction sb_cg_probe_descriptor = &sb_cluster_gateway_abi_v1_get_descriptor;
OpenFunction sb_cg_probe_open = &sb_cluster_gateway_abi_v1_open;
DispatchFunction sb_cg_probe_dispatch = &sb_cluster_gateway_abi_v1_dispatch;
PollFunction sb_cg_probe_poll = &sb_cluster_gateway_abi_v1_poll;
CancelFunction sb_cg_probe_cancel = &sb_cluster_gateway_abi_v1_cancel;
CloseFunction sb_cg_probe_close = &sb_cluster_gateway_abi_v1_close;
#endif

#if defined(__cplusplus)
CHECK_RECORD(SbCgUuidV1);
CHECK_RECORD(SbCgDigestV1);
CHECK_RECORD(SbCgAbiHeaderV1);
CHECK_RECORD(SbCgByteViewV1);
CHECK_RECORD(SbCgMutableBufferV1);
CHECK_RECORD(SbCgDescriptorV1);
CHECK_RECORD(SbCgHostCallRequestV1);
CHECK_RECORD(SbCgHostV1);
CHECK_RECORD(SbCgOpenParamsV1);
CHECK_RECORD(SbCgRequestV1);
CHECK_RECORD(SbCgResultV1);
#endif

int main(void)
{
#if defined(__cplusplus)
    puts("PASS canonical cluster gateway ABI declarations: C++ layout and types");
#else
    puts("PASS canonical cluster gateway ABI declarations: C11 layout and types");
#endif
    return 0;
}
