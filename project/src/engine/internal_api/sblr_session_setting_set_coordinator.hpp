#pragma once
#include "engine/sblr/sblr_session_setting_set_runtime.hpp"
#include "api_types.hpp"

namespace scratchbird::engine::internal_api {
// Issued by the typed server operation and checked by the exact engine root.
// The shared wire carrier does not make different operations interchangeable.
enum class SessionValueOperation : std::uint8_t {
  setting_set, setting_reset, setting_get, default_qualifier_set, discard,
  snapshot_handle, context_set, context_unset, context_get
};
struct SblrSessionSettingSetCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic{};
};
SblrSessionSettingSetCoordinationResult CompileSblrSessionValueDescriptor(
    SessionValueOperation, const EngineRequestContext&, const EngineUuid&,
    uint64_t, uint64_t);
SblrSessionSettingSetCoordinationResult ConsumeSblrSessionValueDescriptor(
    SessionValueOperation, const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1&);
SblrSessionSettingSetCoordinationResult CompileSblrSessionSettingSetDescriptor(
    const EngineRequestContext&, const EngineUuid&, uint64_t, uint64_t);
SblrSessionSettingSetCoordinationResult ConsumeSblrSessionSettingSetDescriptor(
    const EngineRequestContext&,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1&);
// Called when the owning engine receipt has finished retirement.
void RetireSblrSessionSettingSetReceipt(const EngineUuid&);
}  // namespace scratchbird::engine::internal_api
