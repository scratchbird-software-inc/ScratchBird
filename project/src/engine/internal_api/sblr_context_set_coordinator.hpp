#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_context_set_runtime.hpp"

namespace scratchbird::engine::internal_api {
using SblrContextSetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrContextSetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::context_set,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrContextSetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::context_set,
      context, descriptor);
}
using SblrContextUnsetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrContextUnsetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::context_unset,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrContextUnsetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::context_unset,
      context, descriptor);
}
using SblrContextGetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrContextGetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::context_get,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrContextGetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::context_get,
      context, descriptor);
}
}  // namespace scratchbird::engine::internal_api
