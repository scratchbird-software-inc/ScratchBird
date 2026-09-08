#pragma once

#include "api_types.hpp"
#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"

namespace scratchbird::engine::internal_api {

// Neutral engine-owned projection. It contains no parser atoms, session
// record, receipt handle, or authorization object. The receipt binder builds
// it only after resolving the policy and authorizing the exact syntax demand.
struct SblrSecAlterPolicyAuthorityInputV1 {
  scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 descriptor;
};

struct SblrSecAlterPolicyCoordinationResult {
  bool ok = false;
  scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 descriptor{};
  EngineApiDiagnostic diagnostic{};
};

SblrSecAlterPolicyCoordinationResult CompileSblrSecAlterPolicyDescriptor(
    const SblrSecAlterPolicyAuthorityInputV1& authority);

SblrSecAlterPolicyCoordinationResult ValidateSblrSecAlterPolicyDescriptor(
    const SblrSecAlterPolicyAuthorityInputV1& authority,
    const scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1& operand,
    bool cancellation_requested);

}  // namespace scratchbird::engine::internal_api
