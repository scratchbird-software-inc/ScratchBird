#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_session_default_qualifier_runtime.hpp"

namespace scratchbird::engine::internal_api {
using SblrSessionDefaultQualifierSetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionDefaultQualifierSetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::default_qualifier_set,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrSessionDefaultQualifierSetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::default_qualifier_set,
      context, descriptor);
}
}  // namespace scratchbird::engine::internal_api
