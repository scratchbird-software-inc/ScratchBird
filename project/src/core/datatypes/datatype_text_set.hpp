// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "datatype_operations.hpp"
#include <span>
#include <memory_resource>

namespace scratchbird::core::datatypes {
enum class TextSetDuplicatePolicyV1 : std::uint8_t {
  collapse_minimum_bytes = 1, reject = 2, preserve = 3, collapse_first = 4
};
inline constexpr platform::Uuid kNativeTextSetPolicyV1{{
    0x01,0xa1,0x27,0x8c,0x40,0x23,0x7a,0xd0,0x82,0xc6,0x54,0x4a,0xba,0xf0,0x36,0xa5}};
struct NativeTextSetBindingV1 {
  platform::Uuid policy_uuid;
  u64 policy_generation = 0;
  scratchbird::engine::ExecutionTypeDescriptor element;
  DatatypeTextSeedAuthority text_seed;
  bool ordered = false;
  bool allow_null_elements = false;
  TextSetDuplicatePolicyV1 duplicates = TextSetDuplicatePolicyV1::collapse_minimum_bytes;
};
// Resource limits are mechanisms, not memory grants or owning admission.
// The owning executor resolves live resources and retains its grants through
// return. Zero limits do not silently select an unbounded default.
// Inputs and cancellation state must remain valid through return; callbacks
// must not mutate or invalidate borrowed element/frame bytes. Semantic binding
// fields and controls are snapshotted before the first callback.
struct NativeTextSetControlV1 {
  u64 maximum_elements = 0, maximum_value_bytes = 0, maximum_key_bytes = 0;
  u64 maximum_work_bytes = 0, maximum_output_bytes = 0;
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
  // Required upstream for operation scratch. An owning executor supplies its
  // admitted arena adapter; a bare allocator is sufficient only for primitive
  // mechanism tests and is not an engine memory grant.
  std::pmr::memory_resource* memory = nullptr;
};
struct NativeTextSetElementV1 {
  bool is_null = false;
  std::string bytes;
  bool operator==(const NativeTextSetElementV1&) const = default;
};
struct NativeTextSetDecodeResultV1 {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<NativeTextSetElementV1> elements;
  bool ok() const { return status.ok(); }
};
DatatypeSetOperationResult EncodeNativeTextSetV1(
    const NativeTextSetBindingV1&, std::span<const NativeTextSetElementV1>,
    const NativeTextSetControlV1&);
NativeTextSetDecodeResultV1 DecodeNativeTextSetV1(
    const NativeTextSetBindingV1&, std::string_view, const NativeTextSetControlV1&);
DatatypeSetOperationResult ApplyNativeTextSetOperationV1(
    const NativeTextSetBindingV1&, DatatypeSetOperationKind,
    std::string_view left, std::string_view right,
    const NativeTextSetElementV1& member, const NativeTextSetControlV1&);
} // namespace scratchbird::core::datatypes
