#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "limit", "019d0000-0000-7000-8000-000000002551", ".limit",
      {api::kSblrLimitExecutorId, api::kSblrLimitOpcodeCode,
       api::kSblrLimitOpcodeVersion, api::kSblrLimitOperandDescriptorId,
       api::kSblrLimitResultDescriptorId,
       api::kSblrLimitResultDescriptorVersion});
}
