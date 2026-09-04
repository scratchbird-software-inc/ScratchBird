#include "ia01_descriptor_operation_availability_test.hpp"

int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::tests::ia01::RequireMissingExecutorEvidence(
      "project", "019d0000-0000-7000-8000-000000002535", ".project",
      {api::kSblrProjectExecutorId, api::kSblrProjectOpcodeCode,
       api::kSblrProjectOpcodeVersion, api::kSblrProjectOperandDescriptorId,
       api::kSblrProjectResultDescriptorId,
       api::kSblrProjectResultDescriptorVersion});
}
